// Windows backend: the WinRT Bluetooth LE API (Windows 10 1703 and later), called
// through its ABI so it builds with MSVC and MinGW alike, without C++/WinRT or WRL.
//
// Only the interfaces and methods base stations need are declared. Every vtable below
// follows the Windows SDK's generated headers (10.0.19041) slot by slot, slots 0-5 being
// IInspectable; trailing slots nobody calls are left out. combase.dll is loaded at run
// time. Async operations are waited on by polling IAsyncInfo, so no completion
// delegates are needed; the only delegate is the advert watcher's Received handler.
#include "basestation/radio.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <unknwn.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <map>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>

namespace lh::basestation {
namespace {

using RtString = void*;  // HSTRING

struct IRtObject : IUnknown {  // IInspectable
  virtual HRESULT STDMETHODCALLTYPE GetIids(ULONG*, IID**) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetRuntimeClassName(RtString*) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetTrustLevel(int*) = 0;
};

// Windows.Foundation
struct IAsyncInfoRt : IRtObject {
  virtual HRESULT STDMETHODCALLTYPE get_Id(UINT32*) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Status(int*) = 0;  // AsyncStatus
  virtual HRESULT STDMETHODCALLTYPE get_ErrorCode(HRESULT*) = 0;
  virtual HRESULT STDMETHODCALLTYPE Cancel() = 0;
  virtual HRESULT STDMETHODCALLTYPE Close() = 0;
};
enum AsyncStatus { kStarted = 0, kCompleted = 1, kCanceled = 2, kError = 3 };
// IAsyncOperation<T> for an interface pointer T.
struct IAsyncOperationRt : IRtObject {
  virtual HRESULT STDMETHODCALLTYPE put_Completed(void*) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Completed(void**) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetResults(void** results) = 0;
};
// IVectorView<T> and IVector<T> for an interface pointer T: both start GetAt, Size.
struct IVectorViewRt : IRtObject {
  virtual HRESULT STDMETHODCALLTYPE GetAt(UINT32 index, void** item) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Size(UINT32* size) = 0;
};
struct IClosableRt : IRtObject {
  virtual HRESULT STDMETHODCALLTYPE Close() = 0;
};
// IReference<UINT8> (GATT protocol errors).
struct IReferenceByteRt : IRtObject {
  virtual HRESULT STDMETHODCALLTYPE get_Value(BYTE*) = 0;
};

// Windows.Storage.Streams
struct IBufferRt : IRtObject {
  virtual HRESULT STDMETHODCALLTYPE get_Capacity(UINT32*) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Length(UINT32*) = 0;
  virtual HRESULT STDMETHODCALLTYPE put_Length(UINT32) = 0;
};
struct IBufferFactoryRt : IRtObject {
  virtual HRESULT STDMETHODCALLTYPE Create(UINT32 capacity, IBufferRt** value) = 0;
};
struct IBufferByteAccessRt : IUnknown {
  virtual HRESULT STDMETHODCALLTYPE Buffer(BYTE** value) = 0;
};

// Windows.Devices.Bluetooth.Advertisement
struct IAdvertisementRt : IRtObject {
  virtual HRESULT STDMETHODCALLTYPE get_Flags(void**) = 0;
  virtual HRESULT STDMETHODCALLTYPE put_Flags(void*) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_LocalName(RtString*) = 0;
  virtual HRESULT STDMETHODCALLTYPE put_LocalName(RtString) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_ServiceUuids(void**) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_ManufacturerData(void**) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_DataSections(void**) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetManufacturerDataByCompanyId(UINT16 companyId, IVectorViewRt** list) = 0;
};
struct IManufacturerDataRt : IRtObject {
  virtual HRESULT STDMETHODCALLTYPE get_CompanyId(UINT16*) = 0;
  virtual HRESULT STDMETHODCALLTYPE put_CompanyId(UINT16) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Data(IBufferRt**) = 0;
};
struct IAdvertisementReceivedArgsRt : IRtObject {
  virtual HRESULT STDMETHODCALLTYPE get_RawSignalStrengthInDBm(INT16*) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_BluetoothAddress(UINT64*) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_AdvertisementType(int*) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Timestamp(INT64*) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Advertisement(IAdvertisementRt**) = 0;
};
struct IAdvertisementWatcherRt;
// TypedEventHandler<BluetoothLEAdvertisementWatcher, BluetoothLEAdvertisementReceivedEventArgs>
struct IReceivedHandlerRt : IUnknown {
  virtual HRESULT STDMETHODCALLTYPE Invoke(IAdvertisementWatcherRt* sender, IAdvertisementReceivedArgsRt* args) = 0;
};
struct IAdvertisementWatcherRt : IRtObject {
  virtual HRESULT STDMETHODCALLTYPE get_MinSamplingInterval(void*) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_MaxSamplingInterval(void*) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_MinOutOfRangeTimeout(void*) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_MaxOutOfRangeTimeout(void*) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Status(int*) = 0;  // 0 created 1 started 2 stopping 3 stopped 4 aborted
  virtual HRESULT STDMETHODCALLTYPE get_ScanningMode(int*) = 0;
  virtual HRESULT STDMETHODCALLTYPE put_ScanningMode(int) = 0;  // 0 passive, 1 active
  virtual HRESULT STDMETHODCALLTYPE get_SignalStrengthFilter(void**) = 0;
  virtual HRESULT STDMETHODCALLTYPE put_SignalStrengthFilter(void*) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_AdvertisementFilter(void**) = 0;
  virtual HRESULT STDMETHODCALLTYPE put_AdvertisementFilter(void*) = 0;
  virtual HRESULT STDMETHODCALLTYPE Start() = 0;
  virtual HRESULT STDMETHODCALLTYPE Stop() = 0;
  virtual HRESULT STDMETHODCALLTYPE add_Received(IReceivedHandlerRt* handler, INT64* token) = 0;
  virtual HRESULT STDMETHODCALLTYPE remove_Received(INT64 token) = 0;
};

// Windows.Devices.Bluetooth
struct IBluetoothLEDeviceStaticsRt : IRtObject {
  virtual HRESULT STDMETHODCALLTYPE FromIdAsync(RtString, IAsyncOperationRt**) = 0;
  virtual HRESULT STDMETHODCALLTYPE FromBluetoothAddressAsync(UINT64 address, IAsyncOperationRt** op) = 0;
};
struct IBluetoothLEDeviceStatics2Rt : IRtObject {
  virtual HRESULT STDMETHODCALLTYPE GetDeviceSelectorFromPairingState(void*) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetDeviceSelectorFromConnectionStatus(void*) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetDeviceSelectorFromDeviceName(void*) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetDeviceSelectorFromBluetoothAddress(void*) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetDeviceSelectorFromBluetoothAddressWithBluetoothAddressType(void*) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetDeviceSelectorFromAppearance(void*) = 0;
  // addressType: 0 public, 1 random
  virtual HRESULT STDMETHODCALLTYPE FromBluetoothAddressWithBluetoothAddressTypeAsync(UINT64 address, int addressType,
                                                                                     IAsyncOperationRt** op) = 0;
};
struct IBluetoothLEDevice3Rt : IRtObject {
  virtual HRESULT STDMETHODCALLTYPE get_DeviceAccessInformation(void**) = 0;
  virtual HRESULT STDMETHODCALLTYPE RequestAccessAsync(void**) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetGattServicesAsync(void**) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetGattServicesWithCacheModeAsync(int, void**) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetGattServicesForUuidAsync(GUID service, IAsyncOperationRt** op) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetGattServicesForUuidWithCacheModeAsync(GUID service, int cacheMode,
                                                                            IAsyncOperationRt** op) = 0;
};

// Windows.Devices.Bluetooth.GenericAttributeProfile
enum GattStatus { kGattSuccess = 0, kGattUnreachable = 1, kGattProtocolError = 2, kGattAccessDenied = 3 };
enum CacheMode { kCached = 0, kUncached = 1 };
struct IGattDeviceServicesResultRt : IRtObject {
  virtual HRESULT STDMETHODCALLTYPE get_Status(int*) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_ProtocolError(IReferenceByteRt**) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Services(IVectorViewRt**) = 0;
};
struct IGattDeviceService3Rt : IRtObject {
  virtual HRESULT STDMETHODCALLTYPE get_DeviceAccessInformation(void**) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Session(void**) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_SharingMode(void*) = 0;
  virtual HRESULT STDMETHODCALLTYPE RequestAccessAsync(void**) = 0;
  virtual HRESULT STDMETHODCALLTYPE OpenAsync(int, void**) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetCharacteristicsAsync(void**) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetCharacteristicsWithCacheModeAsync(int, void**) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetCharacteristicsForUuidAsync(GUID, IAsyncOperationRt**) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetCharacteristicsForUuidWithCacheModeAsync(GUID characteristic, int cacheMode,
                                                                               IAsyncOperationRt** op) = 0;
};
struct IGattCharacteristicsResultRt : IRtObject {
  virtual HRESULT STDMETHODCALLTYPE get_Status(int*) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_ProtocolError(IReferenceByteRt**) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Characteristics(IVectorViewRt**) = 0;
};
struct IGattCharacteristicRt : IRtObject {
  virtual HRESULT STDMETHODCALLTYPE GetDescriptors(GUID, void**) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_CharacteristicProperties(UINT32*) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_ProtectionLevel(int*) = 0;
  virtual HRESULT STDMETHODCALLTYPE put_ProtectionLevel(int) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_UserDescription(RtString*) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Uuid(GUID*) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_AttributeHandle(UINT16*) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_PresentationFormats(void**) = 0;
  virtual HRESULT STDMETHODCALLTYPE ReadValueAsync(IAsyncOperationRt**) = 0;
  virtual HRESULT STDMETHODCALLTYPE ReadValueWithCacheModeAsync(int cacheMode, IAsyncOperationRt** op) = 0;
};
struct IGattCharacteristic3Rt : IRtObject {
  virtual HRESULT STDMETHODCALLTYPE GetDescriptorsAsync(void**) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetDescriptorsWithCacheModeAsync(int, void**) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetDescriptorsForUuidAsync(GUID, void**) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetDescriptorsForUuidWithCacheModeAsync(GUID, int, void**) = 0;
  virtual HRESULT STDMETHODCALLTYPE WriteValueWithResultAsync(IBufferRt*, IAsyncOperationRt**) = 0;
  // writeOption: 0 with response, 1 without
  virtual HRESULT STDMETHODCALLTYPE WriteValueWithResultAndOptionAsync(IBufferRt* value, int writeOption,
                                                                      IAsyncOperationRt** op) = 0;
};
struct IGattReadResultRt : IRtObject {
  virtual HRESULT STDMETHODCALLTYPE get_Status(int*) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Value(IBufferRt**) = 0;
};
struct IGattWriteResultRt : IRtObject {
  virtual HRESULT STDMETHODCALLTYPE get_Status(int*) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_ProtocolError(IReferenceByteRt**) = 0;
};

// Interface ids (Windows SDK 10.0.19041 headers).
constexpr GUID kIID_IUnknown = {0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr GUID kIID_IAgileObject = {0x94ea2b94, 0xe9cc, 0x49e0, {0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90}};
constexpr GUID kIID_IAsyncInfo = {0x00000036, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr GUID kIID_IClosable = {0x30d5a829, 0x7fa4, 0x4026, {0x83, 0xbb, 0xd7, 0x5b, 0xae, 0x4e, 0xa9, 0x9e}};
constexpr GUID kIID_IBufferFactory = {0x71af914d, 0xc10f, 0x484b, {0xbc, 0x50, 0x14, 0xbc, 0x62, 0x3b, 0x3a, 0x27}};
constexpr GUID kIID_IBufferByteAccess = {0x905a0fef, 0xbc53, 0x11df, {0x8c, 0x49, 0x00, 0x1e, 0x4f, 0xc6, 0x86, 0xda}};
constexpr GUID kIID_IAdvertisementWatcher = {0xa6ac336f, 0xf3d3, 0x4297, {0x8d, 0x6c, 0xc8, 0x1e, 0xa6, 0x62, 0x3f, 0x40}};
constexpr GUID kIID_ReceivedHandler = {0x90eb4eca, 0xd465, 0x5ea0, {0xa6, 0x1c, 0x03, 0x3c, 0x8c, 0x5e, 0xce, 0xf2}};
constexpr GUID kIID_IBluetoothLEDeviceStatics = {0xc8cf1a19, 0xf0b6, 0x4bf0, {0x86, 0x89, 0x41, 0x30, 0x3d, 0xe2, 0xd9, 0xf4}};
constexpr GUID kIID_IBluetoothLEDeviceStatics2 = {0x5f12c06b, 0x3bac, 0x43e8, {0xad, 0x16, 0x56, 0x32, 0x71, 0xbd, 0x41, 0xc2}};
constexpr GUID kIID_IBluetoothLEDevice3 = {0xaee9e493, 0x44ac, 0x40dc, {0xaf, 0x33, 0xb2, 0xc1, 0x3c, 0x01, 0xca, 0x46}};
constexpr GUID kIID_IGattDeviceService3 = {0xb293a950, 0x0c53, 0x437c, {0xa9, 0xb3, 0x5c, 0x32, 0x10, 0xc6, 0xe5, 0x69}};
constexpr GUID kIID_IGattCharacteristic3 = {0x3f3c663e, 0x93d4, 0x406b, {0xb8, 0x17, 0xdb, 0x81, 0xf8, 0xed, 0x53, 0xb3}};

bool SameGuid(const GUID& a, const GUID& b) { return std::memcmp(&a, &b, sizeof(GUID)) == 0; }
GUID ToGuid(const Uuid& u) {
  GUID g;
  g.Data1 = u.data1;
  g.Data2 = u.data2;
  g.Data3 = u.data3;
  std::memcpy(g.Data4, u.data4.data(), 8);
  return g;
}
std::string Hex(HRESULT hr) {
  char text[16];
  std::snprintf(text, sizeof text, "0x%08lX", static_cast<unsigned long>(hr));
  return text;
}

// An owning interface pointer.
template <class T>
class Ref {
 public:
  Ref() = default;
  explicit Ref(T* p) : p_(p) {}
  Ref(const Ref&) = delete;
  Ref& operator=(const Ref&) = delete;
  Ref(Ref&& other) noexcept : p_(std::exchange(other.p_, nullptr)) {}
  Ref& operator=(Ref&& other) noexcept {
    if (this != &other) { Reset(); p_ = std::exchange(other.p_, nullptr); }
    return *this;
  }
  ~Ref() { Reset(); }
  void Reset() { if (p_) std::exchange(p_, nullptr)->Release(); }
  T* Get() const { return p_; }
  T* operator->() const { return p_; }
  explicit operator bool() const { return p_ != nullptr; }
  T** Put() { Reset(); return &p_; }
  void** PutVoid() { return reinterpret_cast<void**>(Put()); }
  template <class U>
  Ref<U> As(const GUID& iid) const {
    Ref<U> out;
    if (p_) p_->QueryInterface(iid, out.PutVoid());
    return out;
  }

 private:
  T* p_ = nullptr;
};

// combase.dll, loaded once.
struct Combase {
  HRESULT(WINAPI* RoInitialize)(int) = nullptr;
  void(WINAPI* RoUninitialize)() = nullptr;
  HRESULT(WINAPI* RoGetActivationFactory)(RtString, REFIID, void**) = nullptr;
  HRESULT(WINAPI* RoActivateInstance)(RtString, IRtObject**) = nullptr;
  HRESULT(WINAPI* WindowsCreateString)(const wchar_t*, UINT32, RtString*) = nullptr;
  HRESULT(WINAPI* WindowsDeleteString)(RtString) = nullptr;
  const wchar_t*(WINAPI* WindowsGetStringRawBuffer)(RtString, UINT32*) = nullptr;
  HRESULT(WINAPI* CoIncrementMTAUsage)(void**) = nullptr;  // Windows 8 and later
  bool ok = false;

  static const Combase& Get() {
    static const Combase instance = [] {
      Combase c;
      HMODULE module = LoadLibraryW(L"combase.dll");
      if (!module) return c;
      auto load = [&](auto& fn, const char* name) {
        fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(reinterpret_cast<void*>(GetProcAddress(module, name)));
        return fn != nullptr;
      };
      c.ok = load(c.RoInitialize, "RoInitialize") && load(c.RoUninitialize, "RoUninitialize") &&
             load(c.RoGetActivationFactory, "RoGetActivationFactory") &&
             load(c.RoActivateInstance, "RoActivateInstance") && load(c.WindowsCreateString, "WindowsCreateString") &&
             load(c.WindowsDeleteString, "WindowsDeleteString") &&
             load(c.WindowsGetStringRawBuffer, "WindowsGetStringRawBuffer");
      // Keeps the multithreaded apartment for the process's life: objects made on a
      // helper thread (OnMta, for callers in a single-threaded apartment) outlive it.
      void* cookie = nullptr;
      if (c.ok && load(c.CoIncrementMTAUsage, "CoIncrementMTAUsage")) c.CoIncrementMTAUsage(&cookie);
      return c;
    }();
    return instance;
  }
};

// A class name as an HSTRING for the activation calls.
class ClassName {
 public:
  explicit ClassName(const wchar_t* name) {
    Combase::Get().WindowsCreateString(name, static_cast<UINT32>(wcslen(name)), &s_);
  }
  ~ClassName() { if (s_) Combase::Get().WindowsDeleteString(s_); }
  RtString Get() const { return s_; }

 private:
  RtString s_ = nullptr;
};

std::string Utf8(RtString s) {
  if (!s) return {};
  UINT32 length = 0;
  const wchar_t* text = Combase::Get().WindowsGetStringRawBuffer(s, &length);
  if (!text || !length) return {};
  const int bytes = WideCharToMultiByte(CP_UTF8, 0, text, int(length), nullptr, 0, nullptr, nullptr);
  std::string out(size_t(bytes > 0 ? bytes : 0), '\0');
  if (bytes > 0) WideCharToMultiByte(CP_UTF8, 0, text, int(length), out.data(), bytes, nullptr, nullptr);
  return out;
}

// WinRT objects need a multithreaded apartment; the calling threads here are our own
// (the CLI's, the driver's helper threads), joined to the MTA on first use.
struct Apartment {
  bool joined = false, usable = false;
  ~Apartment() { if (joined) Combase::Get().RoUninitialize(); }
  bool Ensure() {
    if (usable) return true;
    const HRESULT hr = Combase::Get().RoInitialize(1 /* RO_INIT_MULTITHREADED */);
    joined = hr == S_OK || hr == S_FALSE;
    usable = joined;
    return usable;
  }
};
thread_local Apartment t_apartment;

// Runs `fn` on this thread if it is (or can join) the MTA, else on a helper thread
// (a single-threaded apartment, e.g. a UI thread, would need a message loop). What the
// helper makes stays valid after it: Combase holds the MTA.
template <class F>
auto OnMta(F&& fn) -> decltype(fn()) {
  if (t_apartment.Ensure()) return fn();
  decltype(fn()) result{};
  std::thread([&] {
    t_apartment.Ensure();
    result = fn();
  }).join();
  return result;
}

// Waits for an IAsyncOperation<T*> and takes its result.
template <class T>
bool Await(Ref<IAsyncOperationRt> op, Ref<T>* result, std::chrono::milliseconds timeout, const char* what,
           std::string* error) {
  if (!op) { if (error) *error = std::string(what) + ": not started"; return false; }
  auto info = op.template As<IAsyncInfoRt>(kIID_IAsyncInfo);
  if (!info) { if (error) *error = std::string(what) + ": no IAsyncInfo"; return false; }
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  int status = kStarted;
  while (SUCCEEDED(info->get_Status(&status)) && status == kStarted) {
    if (std::chrono::steady_clock::now() > deadline) {
      info->Cancel();
      if (error) *error = std::string(what) + ": timed out";
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (status == kCompleted) {
    const HRESULT hr = op->GetResults(result->PutVoid());
    info->Close();
    if (FAILED(hr) || !*result) {
      if (error) *error = std::string(what) + (FAILED(hr) ? ": failed " + Hex(hr) : ": nothing found");
      return false;
    }
    return true;
  }
  HRESULT code = S_OK;
  info->get_ErrorCode(&code);
  info->Close();
  if (error) *error = std::string(what) + (status == kCanceled ? ": canceled" : ": failed " + Hex(code));
  return false;
}

std::vector<uint8_t> Bytes(IBufferRt* buffer) {
  std::vector<uint8_t> out;
  if (!buffer) return out;
  UINT32 length = 0;
  buffer->get_Length(&length);
  Ref<IBufferByteAccessRt> access;
  buffer->QueryInterface(kIID_IBufferByteAccess, access.PutVoid());
  BYTE* data = nullptr;
  if (access && SUCCEEDED(access->Buffer(&data)) && data) out.assign(data, data + length);
  return out;
}

Ref<IBufferRt> MakeBuffer(const std::vector<uint8_t>& bytes) {
  Ref<IBufferFactoryRt> factory;
  ClassName name(L"Windows.Storage.Streams.Buffer");
  Ref<IBufferRt> buffer;
  if (FAILED(Combase::Get().RoGetActivationFactory(name.Get(), kIID_IBufferFactory, factory.PutVoid())) || !factory)
    return buffer;
  if (FAILED(factory->Create(UINT32(bytes.size()), buffer.Put())) || !buffer) return Ref<IBufferRt>();
  Ref<IBufferByteAccessRt> access;
  buffer->QueryInterface(kIID_IBufferByteAccess, access.PutVoid());
  BYTE* data = nullptr;
  if (!access || FAILED(access->Buffer(&data)) || !data) return Ref<IBufferRt>();
  if (!bytes.empty()) std::memcpy(data, bytes.data(), bytes.size());
  buffer->put_Length(UINT32(bytes.size()));
  return buffer;
}

std::string GattStatusText(int status, IReferenceByteRt* protocolError) {
  switch (status) {
    case kGattUnreachable: return "unreachable (out of range, or busy with another connection)";
    case kGattAccessDenied: return "access denied";
    case kGattProtocolError: {
      BYTE code = 0;
      if (protocolError) protocolError->get_Value(&code);
      char text[48];
      std::snprintf(text, sizeof text, "protocol error 0x%02X", unsigned(code));
      return text;
    }
    default: return "status " + std::to_string(status);
  }
}

void CloseObject(IUnknown* object) {
  if (!object) return;
  Ref<IClosableRt> closable;
  object->QueryInterface(kIID_IClosable, closable.PutVoid());
  if (closable) closable->Close();
}

// The advert watcher's Received handler.
struct ScanSink {
  std::mutex mutex;
  std::condition_variable wake;
  bool active = true, stop = false;
  const std::function<bool(const Advert&)>* onAdvert = nullptr;
};

class ReceivedHandler final : public IReceivedHandlerRt {
 public:
  explicit ReceivedHandler(std::shared_ptr<ScanSink> sink) : sink_(std::move(sink)) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    if (!out) return E_POINTER;
    if (SameGuid(iid, kIID_IUnknown) || SameGuid(iid, kIID_ReceivedHandler) || SameGuid(iid, kIID_IAgileObject)) {
      *out = static_cast<IReceivedHandlerRt*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG left = --refs_;
    if (!left) delete this;
    return left;
  }
  HRESULT STDMETHODCALLTYPE Invoke(IAdvertisementWatcherRt*, IAdvertisementReceivedArgsRt* args) override {
    if (!args) return S_OK;
    Advert advert;
    UINT64 address = 0;
    INT16 rssi = 0;
    args->get_BluetoothAddress(&address);
    args->get_RawSignalStrengthInDBm(&rssi);
    advert.address = address;
    advert.rssi = rssi;
    Ref<IAdvertisementRt> ad;
    if (SUCCEEDED(args->get_Advertisement(ad.Put())) && ad) {
      RtString name = nullptr;
      if (SUCCEEDED(ad->get_LocalName(&name)) && name) {
        advert.name = Utf8(name);
        Combase::Get().WindowsDeleteString(name);
      }
      Ref<IVectorViewRt> list;
      UINT32 count = 0;
      if (SUCCEEDED(ad->GetManufacturerDataByCompanyId(kValveCompanyId, list.Put())) && list &&
          SUCCEEDED(list->get_Size(&count)) && count > 0) {
        Ref<IManufacturerDataRt> data;
        Ref<IBufferRt> buffer;
        if (SUCCEEDED(list->GetAt(0, data.PutVoid())) && data && SUCCEEDED(data->get_Data(buffer.Put())) && buffer) {
          advert.hasValveData = true;
          advert.valveData = Bytes(buffer.Get());
        }
      }
    }
    std::lock_guard lock(sink_->mutex);
    if (sink_->active && sink_->onAdvert && !(*sink_->onAdvert)(advert)) {
      sink_->stop = true;
      sink_->wake.notify_all();
    }
    return S_OK;
  }

 private:
  std::atomic<ULONG> refs_{1};
  std::shared_ptr<ScanSink> sink_;
};

class WinRtLink final : public GattLink {
 public:
  // connectBy: when opening the link and finding its services has to be done.
  WinRtLink(Ref<IRtObject> device, std::chrono::steady_clock::time_point connectBy)
      : device_(std::move(device)), connectBy_(connectBy) {}
  ~WinRtLink() override {
    OnMta([&] {
      characteristics_.clear();
      for (auto& [key, service] : services_) CloseObject(service.Get());
      services_.clear();
      CloseObject(device_.Get());  // drops the connection
      device_.Reset();
      return 0;
    });
  }

  bool Read(const Uuid& service, const Uuid& characteristic, std::vector<uint8_t>* value, std::string* error) override {
    return OnMta([&] {
      Ref<IGattCharacteristicRt> c;
      if (!Characteristic(service, characteristic, &c, error)) return false;
      Ref<IAsyncOperationRt> op;
      if (FAILED(c->ReadValueWithCacheModeAsync(kUncached, op.Put()))) return Fail(error, "read: cannot start");
      Ref<IGattReadResultRt> result;
      if (!Await(std::move(op), &result, kTransferTimeout, "read", error)) return false;
      int status = -1;
      result->get_Status(&status);
      if (status != kGattSuccess) return Fail(error, "read: " + GattStatusText(status, nullptr));
      Ref<IBufferRt> buffer;
      result->get_Value(buffer.Put());
      if (value) *value = Bytes(buffer.Get());
      return true;
    });
  }

  bool Write(const Uuid& service, const Uuid& characteristic, const std::vector<uint8_t>& value,
             std::string* error) override {
    return OnMta([&] {
      Ref<IGattCharacteristicRt> c;
      if (!Characteristic(service, characteristic, &c, error)) return false;
      auto c3 = c.As<IGattCharacteristic3Rt>(kIID_IGattCharacteristic3);
      Ref<IBufferRt> buffer = MakeBuffer(value);
      if (!c3 || !buffer) return Fail(error, "write: WinRT objects unavailable");
      Ref<IAsyncOperationRt> op;
      if (FAILED(c3->WriteValueWithResultAndOptionAsync(buffer.Get(), 0 /* with response */, op.Put())))
        return Fail(error, "write: cannot start");
      Ref<IGattWriteResultRt> result;
      if (!Await(std::move(op), &result, kTransferTimeout, "write", error)) return false;
      int status = -1;
      result->get_Status(&status);
      if (status != kGattSuccess) {
        Ref<IReferenceByteRt> protocolError;
        result->get_ProtocolError(protocolError.Put());
        return Fail(error, "write: " + GattStatusText(status, protocolError.Get()));
      }
      return true;
    });
  }

 private:
  static bool Fail(std::string* error, const std::string& text) {
    if (error) *error = text;
    return false;
  }

  // The service's first match, from Windows' cache when it has one.
  bool Service(const Uuid& uuid, Ref<IRtObject>** out, std::string* error) {
    const std::string key = ToString(uuid);
    if (auto found = services_.find(key); found != services_.end()) { *out = &found->second; return true; }
    auto device3 = device_.As<IBluetoothLEDevice3Rt>(kIID_IBluetoothLEDevice3);
    if (!device3) return Fail(error, "this Windows has no GATT client (needs Windows 10 1703 or later)");
    std::string why;
    for (int cacheMode : {kCached, kUncached}) {
      Ref<IAsyncOperationRt> op;
      if (FAILED(device3->GetGattServicesForUuidWithCacheModeAsync(ToGuid(uuid), cacheMode, op.Put()))) continue;
      Ref<IGattDeviceServicesResultRt> result;
      if (!Await(std::move(op), &result, Left(), "connect", &why)) continue;
      int status = -1;
      result->get_Status(&status);
      Ref<IVectorViewRt> list;
      UINT32 count = 0;
      if (status != kGattSuccess) { why = "connect: " + GattStatusText(status, nullptr); continue; }
      if (FAILED(result->get_Services(list.Put())) || !list || FAILED(list->get_Size(&count)) || !count) {
        why = "service " + key + " not found";
        continue;
      }
      Ref<IRtObject> service;
      if (FAILED(list->GetAt(0, service.PutVoid())) || !service) continue;
      *out = &(services_[key] = std::move(service));
      return true;
    }
    return Fail(error, why.empty() ? "connect failed" : why);
  }

  bool Characteristic(const Uuid& serviceUuid, const Uuid& uuid, Ref<IGattCharacteristicRt>* out, std::string* error) {
    const std::string key = ToString(serviceUuid) + "/" + ToString(uuid);
    if (auto found = characteristics_.find(key); found != characteristics_.end()) {
      found->second->AddRef();
      *out = Ref<IGattCharacteristicRt>(found->second.Get());
      return true;
    }
    Ref<IRtObject>* service = nullptr;
    if (!Service(serviceUuid, &service, error)) return false;
    auto service3 = service->As<IGattDeviceService3Rt>(kIID_IGattDeviceService3);
    if (!service3) return Fail(error, "this Windows has no GATT client (needs Windows 10 1703 or later)");
    std::string why;
    for (int cacheMode : {kCached, kUncached}) {
      Ref<IAsyncOperationRt> op;
      if (FAILED(service3->GetCharacteristicsForUuidWithCacheModeAsync(ToGuid(uuid), cacheMode, op.Put()))) continue;
      Ref<IGattCharacteristicsResultRt> result;
      if (!Await(std::move(op), &result, Left(), "characteristic", &why)) continue;
      int status = -1;
      result->get_Status(&status);
      Ref<IVectorViewRt> list;
      UINT32 count = 0;
      if (status != kGattSuccess) { why = "characteristic: " + GattStatusText(status, nullptr); continue; }
      if (FAILED(result->get_Characteristics(list.Put())) || !list || FAILED(list->get_Size(&count)) || !count) {
        why = "characteristic " + ToString(uuid) + " not found";
        continue;
      }
      Ref<IGattCharacteristicRt> c;
      if (FAILED(list->GetAt(0, c.PutVoid())) || !c) continue;
      c->AddRef();
      characteristics_[key] = Ref<IGattCharacteristicRt>(c.Get());
      *out = std::move(c);
      return true;
    }
    return Fail(error, why.empty() ? "characteristic not found" : why);
  }

  // What's left for connecting; characteristics looked up later (another service) get a
  // few seconds at least.
  std::chrono::milliseconds Left() const {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(connectBy_ - std::chrono::steady_clock::now());
    return std::max(left, std::chrono::milliseconds(3000));
  }
  static constexpr std::chrono::milliseconds kTransferTimeout{5000};  // one read or write on an open link

  Ref<IRtObject> device_;
  std::chrono::steady_clock::time_point connectBy_;
  std::map<std::string, Ref<IRtObject>> services_;
  std::map<std::string, Ref<IGattCharacteristicRt>> characteristics_;
};

class WinRtRadio final : public Radio {
 public:
  bool Scan(std::chrono::milliseconds duration, const std::function<bool(const Advert&)>& onAdvert,
            std::string* error) override {
    return OnMta([&] {
      ClassName name(L"Windows.Devices.Bluetooth.Advertisement.BluetoothLEAdvertisementWatcher");
      Ref<IRtObject> object;
      if (FAILED(Combase::Get().RoActivateInstance(name.Get(), object.Put())) || !object) {
        if (error) *error = "Bluetooth LE scanning is unavailable on this Windows";
        return false;
      }
      auto watcher = object.As<IAdvertisementWatcherRt>(kIID_IAdvertisementWatcher);
      if (!watcher) { if (error) *error = "Bluetooth LE scanning is unavailable on this Windows"; return false; }
      watcher->put_ScanningMode(1);  // active: names can come in scan responses
      auto sink = std::make_shared<ScanSink>();
      sink->onAdvert = &onAdvert;
      auto* handler = new ReceivedHandler(sink);
      INT64 token = 0;
      HRESULT hr = watcher->add_Received(handler, &token);
      handler->Release();  // the watcher holds it now
      if (FAILED(hr)) { if (error) *error = "cannot listen for adverts (" + Hex(hr) + ")"; return false; }
      hr = watcher->Start();
      bool ok = SUCCEEDED(hr);
      if (!ok && error) *error = "cannot start a Bluetooth scan (" + Hex(hr) + "): is Bluetooth on?";
      const auto deadline = std::chrono::steady_clock::now() + duration;
      while (ok) {
        std::unique_lock lock(sink->mutex);
        if (sink->stop || sink->wake.wait_until(lock, std::min(deadline, std::chrono::steady_clock::now() +
                                                                             std::chrono::milliseconds(100)),
                                                [&] { return sink->stop; }))
          break;
        lock.unlock();
        if (std::chrono::steady_clock::now() >= deadline) break;
        int status = 0;
        if (SUCCEEDED(watcher->get_Status(&status)) && status == 4) {  // aborted: radio off or gone
          ok = false;
          if (error) *error = "the Bluetooth scan was aborted: is Bluetooth on?";
        }
      }
      watcher->Stop();
      watcher->remove_Received(token);
      std::lock_guard lock(sink->mutex);
      sink->active = false;  // late adverts no longer reach onAdvert
      return ok;
    });
  }

  std::unique_ptr<GattLink> Connect(uint64_t address, std::chrono::milliseconds timeout, std::string* error) override {
    return OnMta([&]() -> std::unique_ptr<GattLink> {
      const auto connectBy = std::chrono::steady_clock::now() + timeout;
      auto left = [&] {
        return std::max(std::chrono::duration_cast<std::chrono::milliseconds>(connectBy - std::chrono::steady_clock::now()),
                        std::chrono::milliseconds(1000));
      };
      ClassName name(L"Windows.Devices.Bluetooth.BluetoothLEDevice");
      Ref<IRtObject> device;
      std::string why = "Bluetooth LE is unavailable on this Windows";
      // Base stations use random static addresses (top bits 11).
      Ref<IBluetoothLEDeviceStatics2Rt> statics2;
      Combase::Get().RoGetActivationFactory(name.Get(), kIID_IBluetoothLEDeviceStatics2, statics2.PutVoid());
      if (statics2) {
        Ref<IAsyncOperationRt> op;
        const int type = (address >> 46) == 3 ? 1 : 0;
        if (SUCCEEDED(statics2->FromBluetoothAddressWithBluetoothAddressTypeAsync(address, type, op.Put())))
          Await(std::move(op), &device, left(), "open", &why);
      }
      if (!device) {
        Ref<IBluetoothLEDeviceStaticsRt> statics;
        Combase::Get().RoGetActivationFactory(name.Get(), kIID_IBluetoothLEDeviceStatics, statics.PutVoid());
        Ref<IAsyncOperationRt> op;
        if (statics && SUCCEEDED(statics->FromBluetoothAddressAsync(address, op.Put())))
          Await(std::move(op), &device, left(), "open", &why);
      }
      if (!device) {
        if (error) *error = why;
        return nullptr;
      }
      return std::make_unique<WinRtLink>(std::move(device), connectBy);
    });
  }
};

}  // namespace

std::unique_ptr<Radio> OpenPlatformRadio(std::string* error) {
  if (!Combase::Get().ok) {
    if (error) *error = "WinRT is unavailable (combase.dll)";
    return nullptr;
  }
  return std::make_unique<WinRtRadio>();
}

}  // namespace lh::basestation

#else  // !_WIN32

namespace lh::basestation {
// Linux (BlueZ over D-Bus) and macOS (CoreBluetooth) aren't written yet: ai/driver/basestation_power.md.
std::unique_ptr<Radio> OpenPlatformRadio(std::string* error) {
  if (error) *error = "base station Bluetooth control is Windows-only so far";
  return nullptr;
}
}  // namespace lh::basestation

#endif
