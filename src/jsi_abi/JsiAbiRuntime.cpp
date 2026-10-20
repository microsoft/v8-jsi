/*
 * Copyright (c) Microsoft Corporation.
 * Licensed under the MIT license.
 *
 * Portions derived from facebook/hermes (Hermes ABI):
 *   Copyright (c) Meta Platforms, Inc. and affiliates.
 *   Licensed under the MIT license.
 *
 * JsiAbiRuntime — C++ wrapper that wraps the jsi_runtime C interface
 * into a facebook::jsi::Runtime C++ class.
 *
 * Modeled after HermesABIRuntimeWrapper.cpp with jsi_* naming.
 */

#include "jsi_abi/JsiAbiRuntime.h"

#include "jsi_abi/jsi_abi.h"
#include "jsi_abi/jsi_abi_helpers.h"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace jsi::abi {

//==============================================================================
// StringByteBuffer — growable buffer backed by std::string
//==============================================================================

namespace {

struct StringByteBuffer : public jsi_growable_buffer {
  std::string buf_;

  StringByteBuffer() : buf_(256, '\0') {
    static const jsi_growable_buffer_api bufVt{&tryGrowTo};
    api = const_cast<jsi_growable_buffer_api *>(&bufVt);
    data = reinterpret_cast<uint8_t *>(buf_.data());
    size = buf_.size();
    used = 0;
  }

  static void JSI_ABI_CDECL tryGrowTo(jsi_growable_buffer *buf, size_t sz) {
    auto *self = static_cast<StringByteBuffer *>(buf);
    if (sz > self->buf_.size()) {
      self->buf_.resize(sz);
      buf->data = reinterpret_cast<uint8_t *>(self->buf_.data());
      buf->size = self->buf_.size();
    }
  }

  std::string get() && {
    buf_.resize(used);
    return std::move(buf_);
  }
};

//==============================================================================
// SaveAndRestore — RAII helper to save and restore a value
//==============================================================================

template <typename T>
struct SaveAndRestore {
  T &target_;
  T oldVal_;
  explicit SaveAndRestore(T &target) : target_(target), oldVal_(target) {}
  ~SaveAndRestore() { target_ = oldVal_; }
};

//==============================================================================
// BufferWrapper — wraps facebook::jsi::Buffer as jsi_buffer
//==============================================================================

struct BufferWrapper : public jsi_buffer {
  std::shared_ptr<const facebook::jsi::Buffer> buf_;

  explicit BufferWrapper(std::shared_ptr<const facebook::jsi::Buffer> buf)
      : buf_(std::move(buf)) {
    static const jsi_buffer_api bufVt{&release};
    api = const_cast<jsi_buffer_api *>(&bufVt);
    data = buf_->data();
    size = buf_->size();
  }

  static void JSI_ABI_CDECL release(jsi_buffer *self) {
    delete static_cast<BufferWrapper *>(self);
  }
};

//==============================================================================
// MutableBufferWrapper — wraps facebook::jsi::MutableBuffer
//==============================================================================

struct MutableBufferWrapper : public jsi_mutable_buffer {
  std::shared_ptr<facebook::jsi::MutableBuffer> buf_;

  explicit MutableBufferWrapper(
      std::shared_ptr<facebook::jsi::MutableBuffer> buf)
      : buf_(std::move(buf)) {
    static const jsi_mutable_buffer_api bufVt{&release};
    api = const_cast<jsi_mutable_buffer_api *>(&bufVt);
    data = buf_->data();
    size = buf_->size();
  }

  static void JSI_ABI_CDECL release(jsi_mutable_buffer *self) {
    delete static_cast<MutableBufferWrapper *>(self);
  }
};

//==============================================================================
// PreparedJSWrapper — wraps a jsi_prepared_javascript handle
//==============================================================================

struct PreparedJSWrapper : public facebook::jsi::PreparedJavaScript {
  jsi_prepared_javascript *prepared;
  const jsi_runtime_api *api;
  jsi_runtime *rt;

  ~PreparedJSWrapper() override {
    prepared->api->release(prepared);
  }
};

//==============================================================================
// JsiAbiRuntime — facebook::jsi::Runtime implementation over the JSI ABI.
// Internal to this TU; consumers see only the factory functions declared in
// JsiAbiRuntime.h.
//==============================================================================

class JsiAbiRuntime : public facebook::jsi::Runtime {
 public:
  // Adopts one ref on abiRuntime. Destructor calls release on abiRuntime->api.
  explicit JsiAbiRuntime(jsi_runtime *abiRuntime);

  ~JsiAbiRuntime() override;

  JsiAbiRuntime(const JsiAbiRuntime &) = delete;
  JsiAbiRuntime &operator=(const JsiAbiRuntime &) = delete;

  facebook::jsi::Value evaluateJavaScript(
      const std::shared_ptr<const facebook::jsi::Buffer> &buffer,
      const std::string &sourceURL) override;

  std::shared_ptr<const facebook::jsi::PreparedJavaScript> prepareJavaScript(
      const std::shared_ptr<const facebook::jsi::Buffer> &buffer,
      std::string sourceURL) override;

  facebook::jsi::Value evaluatePreparedJavaScript(
      const std::shared_ptr<const facebook::jsi::PreparedJavaScript> &js)
      override;

#if JSI_VERSION >= 4
  bool drainMicrotasks(int maxMicrotasksHint = -1) override;
#endif

#if JSI_VERSION >= 12
  void queueMicrotask(const facebook::jsi::Function &callback) override;
#endif

  facebook::jsi::Object global() override;
  std::string description() override;
  bool isInspectable() override;

  jsi_runtime *abiRuntime() const noexcept { return abiRt_; }

 protected:
  PointerValue *cloneSymbol(const PointerValue *pv) override;
  PointerValue *cloneString(const PointerValue *pv) override;
#if JSI_VERSION >= 6
  PointerValue *cloneBigInt(const PointerValue *pv) override;
#endif
  PointerValue *cloneObject(const PointerValue *pv) override;
  PointerValue *clonePropNameID(const PointerValue *pv) override;

  facebook::jsi::PropNameID createPropNameIDFromAscii(
      const char *str,
      size_t length) override;
  facebook::jsi::PropNameID createPropNameIDFromUtf8(
      const uint8_t *utf8,
      size_t length) override;
  facebook::jsi::PropNameID createPropNameIDFromString(
      const facebook::jsi::String &str) override;
#if JSI_VERSION >= 5
  facebook::jsi::PropNameID createPropNameIDFromSymbol(
      const facebook::jsi::Symbol &sym) override;
#endif
  std::string utf8(const facebook::jsi::PropNameID &) override;
  bool compare(
      const facebook::jsi::PropNameID &,
      const facebook::jsi::PropNameID &) override;

  std::string symbolToString(const facebook::jsi::Symbol &) override;

#if JSI_VERSION >= 8
  facebook::jsi::BigInt createBigIntFromInt64(int64_t) override;
  facebook::jsi::BigInt createBigIntFromUint64(uint64_t) override;
  bool bigintIsInt64(const facebook::jsi::BigInt &) override;
  bool bigintIsUint64(const facebook::jsi::BigInt &) override;
  uint64_t truncate(const facebook::jsi::BigInt &) override;
  facebook::jsi::String bigintToString(
      const facebook::jsi::BigInt &,
      int) override;
#endif

  facebook::jsi::String createStringFromAscii(
      const char *str,
      size_t length) override;
  facebook::jsi::String createStringFromUtf8(
      const uint8_t *utf8,
      size_t length) override;
  std::string utf8(const facebook::jsi::String &) override;

  facebook::jsi::Object createObject() override;
  facebook::jsi::Object createObject(
      std::shared_ptr<facebook::jsi::HostObject> ho) override;
  std::shared_ptr<facebook::jsi::HostObject> getHostObject(
      const facebook::jsi::Object &) override;
  facebook::jsi::HostFunctionType &getHostFunction(
      const facebook::jsi::Function &) override;

#if JSI_VERSION >= 7
  bool hasNativeState(const facebook::jsi::Object &) override;
  std::shared_ptr<facebook::jsi::NativeState> getNativeState(
      const facebook::jsi::Object &) override;
  void setNativeState(
      const facebook::jsi::Object &,
      std::shared_ptr<facebook::jsi::NativeState> state) override;
#endif

#if JSI_VERSION >= 17
  void setPrototypeOf(
      const facebook::jsi::Object &object,
      const facebook::jsi::Value &prototype) override;
  facebook::jsi::Value getPrototypeOf(
      const facebook::jsi::Object &object) override;
#endif

  facebook::jsi::Value getProperty(
      const facebook::jsi::Object &,
      const facebook::jsi::PropNameID &name) override;
  facebook::jsi::Value getProperty(
      const facebook::jsi::Object &,
      const facebook::jsi::String &name) override;
  bool hasProperty(
      const facebook::jsi::Object &,
      const facebook::jsi::PropNameID &name) override;
  bool hasProperty(
      const facebook::jsi::Object &,
      const facebook::jsi::String &name) override;
  void setPropertyValue(
      JSI_CONST_10 facebook::jsi::Object &,
      const facebook::jsi::PropNameID &name,
      const facebook::jsi::Value &value) override;
  void setPropertyValue(
      JSI_CONST_10 facebook::jsi::Object &,
      const facebook::jsi::String &name,
      const facebook::jsi::Value &value) override;

  bool isArray(const facebook::jsi::Object &) const override;
  bool isArrayBuffer(const facebook::jsi::Object &) const override;
  bool isFunction(const facebook::jsi::Object &) const override;
  bool isHostObject(const facebook::jsi::Object &) const override;
  bool isHostFunction(const facebook::jsi::Function &) const override;
  facebook::jsi::Array getPropertyNames(
      const facebook::jsi::Object &) override;

  facebook::jsi::WeakObject createWeakObject(
      const facebook::jsi::Object &) override;
  facebook::jsi::Value lockWeakObject(
      JSI_NO_CONST_3 JSI_CONST_10 facebook::jsi::WeakObject &) override;

  facebook::jsi::Array createArray(size_t length) override;
#if JSI_VERSION >= 9
  facebook::jsi::ArrayBuffer createArrayBuffer(
      std::shared_ptr<facebook::jsi::MutableBuffer> buffer) override;
#endif
  size_t size(const facebook::jsi::Array &) override;
  size_t size(const facebook::jsi::ArrayBuffer &) override;
  uint8_t *data(const facebook::jsi::ArrayBuffer &) override;
  facebook::jsi::Value getValueAtIndex(
      const facebook::jsi::Array &,
      size_t i) override;
  void setValueAtIndexImpl(
      JSI_CONST_10 facebook::jsi::Array &,
      size_t i,
      const facebook::jsi::Value &value) override;

  facebook::jsi::Function createFunctionFromHostFunction(
      const facebook::jsi::PropNameID &name,
      unsigned int paramCount,
      facebook::jsi::HostFunctionType func) override;
  facebook::jsi::Value call(
      const facebook::jsi::Function &,
      const facebook::jsi::Value &jsThis,
      const facebook::jsi::Value *args,
      size_t count) override;
  facebook::jsi::Value callAsConstructor(
      const facebook::jsi::Function &,
      const facebook::jsi::Value *args,
      size_t count) override;

  bool strictEquals(
      const facebook::jsi::Symbol &a,
      const facebook::jsi::Symbol &b) const override;
#if JSI_VERSION >= 6
  bool strictEquals(
      const facebook::jsi::BigInt &a,
      const facebook::jsi::BigInt &b) const override;
#endif
  bool strictEquals(
      const facebook::jsi::String &a,
      const facebook::jsi::String &b) const override;
  bool strictEquals(
      const facebook::jsi::Object &a,
      const facebook::jsi::Object &b) const override;

  bool instanceOf(
      const facebook::jsi::Object &o,
      const facebook::jsi::Function &f) override;

#if JSI_VERSION >= 11
  void setExternalMemoryPressure(
      const facebook::jsi::Object &,
      size_t) override;
#endif

  ScopeState *pushScope() override;
  void popScope(ScopeState *) override;

 private:
  class ManagedPointerHolder;
  class HostFunctionWrapper;
  class HostObjectWrapper;
  class NativeStateWrapper;

  jsi_pointer *getABIPointer(const PointerValue *pv) const;

  jsi_object toABIObject(const facebook::jsi::Object &obj) const;
  jsi_string toABIString(const facebook::jsi::String &str) const;
  jsi_symbol toABISymbol(const facebook::jsi::Symbol &sym) const;
  jsi_propnameid toABIPropNameID(
      const facebook::jsi::PropNameID &name) const;
  jsi_function toABIFunction(const facebook::jsi::Function &fn) const;
  jsi_array toABIArray(const facebook::jsi::Array &arr) const;
  jsi_arraybuffer toABIArrayBuffer(
      const facebook::jsi::ArrayBuffer &ab) const;
  jsi_weak_object toABIWeakObject(
      const facebook::jsi::WeakObject &wo) const;
#if JSI_VERSION >= 6
  jsi_bigint toABIBigInt(const facebook::jsi::BigInt &bi) const;
#endif

  jsi_value toABIValue(const facebook::jsi::Value &val) const;
  jsi_value cloneToABIValue(const facebook::jsi::Value &val) const;
  facebook::jsi::Value cloneToJSIValue(const jsi_value &val);
  facebook::jsi::PropNameID cloneToJSIPropNameID(jsi_propnameid name);
  facebook::jsi::Value intoJSIValue(jsi_value val);

  [[noreturn]] void throwError(jsi_status err);
  void checkStatus(jsi_status err);

  template <typename Fn>
  jsi_status abiRethrow(Fn fn);

  const jsi_runtime_api *api_;
  jsi_runtime *abiRt_;
  bool activeJSError_ = false;
};

} // namespace

//==============================================================================
// ManagedPointerHolder — nested inside JsiAbiRuntime
// Bridges PointerValue to jsi_pointer with refcounting.
//==============================================================================

class JsiAbiRuntime::ManagedPointerHolder : public PointerValue {
  jsi_pointer *pointer_;
  std::atomic<uint32_t> refCount_{1};

 public:
  explicit ManagedPointerHolder(jsi_pointer *pointer) : pointer_(pointer) {}

  jsi_pointer *pointer() const {
    return pointer_;
  }

  void inc() noexcept {
    refCount_.fetch_add(1, std::memory_order_relaxed);
  }

  void invalidate() JSI_NOEXCEPT_15 override {
    auto oldCount = refCount_.fetch_sub(1, std::memory_order_acq_rel);
    if (oldCount == 1) {
      if (pointer_) {
        pointer_->api->invalidate(pointer_);
      }
      delete this;
    }
  }
};

//==============================================================================
// HostFunctionWrapper — nested inside JsiAbiRuntime
//==============================================================================

class JsiAbiRuntime::HostFunctionWrapper : public jsi_host_function {
  JsiAbiRuntime &rtw_;
  facebook::jsi::HostFunctionType hf_;

 public:
  static const jsi_host_function_api *getVt() {
    static const jsi_host_function_api hfVt{&release, &call};
    return &hfVt;
  }

  HostFunctionWrapper(
      JsiAbiRuntime &rt,
      facebook::jsi::HostFunctionType hf)
      : rtw_(rt), hf_(std::move(hf)) {
    api = const_cast<jsi_host_function_api *>(getVt());
  }

  facebook::jsi::HostFunctionType &getHostFunction() {
    return hf_;
  }

  static void JSI_ABI_CDECL release(jsi_host_function *self) {
    delete static_cast<HostFunctionWrapper *>(self);
  }

  static jsi_status JSI_ABI_CDECL call(
      jsi_host_function *self,
      jsi_runtime * /*abiRt*/,
      const jsi_value *thisArg,
      const jsi_value *args,
      size_t argCount,
      jsi_value *outValue) {
    *outValue = abi::create_undefined_value();
    auto *wrapper = static_cast<HostFunctionWrapper *>(self);
    auto &rtw = wrapper->rtw_;
    return rtw.abiRethrow([&]() -> jsi_status {
      facebook::jsi::Value jsThis = rtw.cloneToJSIValue(*thisArg);
      std::vector<facebook::jsi::Value> jsArgs;
      jsArgs.reserve(argCount);
      for (size_t i = 0; i < argCount; ++i)
        jsArgs.push_back(rtw.cloneToJSIValue(args[i]));

      facebook::jsi::Value result =
          wrapper->hf_(rtw, jsThis, jsArgs.data(), argCount);

      *outValue = rtw.cloneToABIValue(result);
      return jsi_status_ok;
    });
  }
};

//==============================================================================
// HostObjectWrapper — nested inside JsiAbiRuntime
//==============================================================================

class JsiAbiRuntime::HostObjectWrapper : public jsi_host_object {
  JsiAbiRuntime &rtw_;
  std::shared_ptr<facebook::jsi::HostObject> ho_;

  struct PropNameIDListWrapper : public jsi_propnameid_list {
    std::vector<facebook::jsi::PropNameID> jsiProps_;
    std::vector<jsi_propnameid> abiProps_;

    PropNameIDListWrapper(
        std::vector<facebook::jsi::PropNameID> jsiProps,
        std::vector<jsi_propnameid> abiProps)
        : jsiProps_(std::move(jsiProps)), abiProps_(std::move(abiProps)) {
      static const jsi_propnameid_list_api listVt{&release};
      api = const_cast<jsi_propnameid_list_api *>(&listVt);
      props = abiProps_.data();
      size = abiProps_.size();
    }

    static void JSI_ABI_CDECL release(jsi_propnameid_list *self) {
      delete static_cast<PropNameIDListWrapper *>(self);
    }
  };

 public:
  static const jsi_host_object_api *getVt() {
    static const jsi_host_object_api hoVt{
        &release, &get, &set, &getOwnKeys};
    return &hoVt;
  }

  HostObjectWrapper(
      JsiAbiRuntime &rt,
      std::shared_ptr<facebook::jsi::HostObject> ho)
      : rtw_(rt), ho_(std::move(ho)) {
    api = const_cast<jsi_host_object_api *>(getVt());
  }

  std::shared_ptr<facebook::jsi::HostObject> getHostObject() const {
    return ho_;
  }

  static void JSI_ABI_CDECL release(jsi_host_object *self) {
    delete static_cast<HostObjectWrapper *>(self);
  }

  static jsi_status JSI_ABI_CDECL get(
      jsi_host_object *self,
      jsi_runtime * /*abiRt*/,
      jsi_propnameid name,
      jsi_value *outValue) {
    *outValue = abi::create_undefined_value();
    auto *wrapper = static_cast<HostObjectWrapper *>(self);
    auto &rtw = wrapper->rtw_;
    return rtw.abiRethrow([&]() -> jsi_status {
      facebook::jsi::PropNameID jsiName =
          rtw.cloneToJSIPropNameID(name);
      facebook::jsi::Value result = wrapper->ho_->get(rtw, jsiName);
      *outValue = rtw.cloneToABIValue(result);
      return jsi_status_ok;
    });
  }

  static jsi_status JSI_ABI_CDECL set(
      jsi_host_object *self,
      jsi_runtime * /*abiRt*/,
      jsi_propnameid name,
      const jsi_value *value) {
    auto *wrapper = static_cast<HostObjectWrapper *>(self);
    auto &rtw = wrapper->rtw_;
    return rtw.abiRethrow([&]() -> jsi_status {
      facebook::jsi::PropNameID jsiName =
          rtw.cloneToJSIPropNameID(name);
      facebook::jsi::Value jsiVal = rtw.cloneToJSIValue(*value);
      wrapper->ho_->set(rtw, jsiName, jsiVal);
      return jsi_status_ok;
    });
  }

  static jsi_status JSI_ABI_CDECL getOwnKeys(
      jsi_host_object *self,
      jsi_runtime * /*abiRt*/,
      jsi_propnameid_list **outList) {
    *outList = nullptr;
    auto *wrapper = static_cast<HostObjectWrapper *>(self);
    auto &rtw = wrapper->rtw_;
    return rtw.abiRethrow([&]() -> jsi_status {
      auto names = wrapper->ho_->getPropertyNames(rtw);
      std::vector<jsi_propnameid> abiProps;
      abiProps.reserve(names.size());
      for (auto &n : names) {
        abiProps.push_back({rtw.getABIPointer(getPointerValue(n))});
      }
      *outList = new PropNameIDListWrapper(
          std::move(names), std::move(abiProps));
      return jsi_status_ok;
    });
  }
};

//==============================================================================
// NativeStateWrapper — nested inside JsiAbiRuntime
//==============================================================================

class JsiAbiRuntime::NativeStateWrapper : public jsi_native_state {
  std::shared_ptr<facebook::jsi::NativeState> state_;

 public:
  static const jsi_native_state_api *getVt() {
    static const jsi_native_state_api nsVt{&release};
    return &nsVt;
  }

  explicit NativeStateWrapper(
      std::shared_ptr<facebook::jsi::NativeState> state)
      : state_(std::move(state)) {
    api = const_cast<jsi_native_state_api *>(getVt());
  }

  std::shared_ptr<facebook::jsi::NativeState> getNativeState() const {
    return state_;
  }

  static void JSI_ABI_CDECL release(jsi_native_state *self) {
    delete static_cast<NativeStateWrapper *>(self);
  }
};

//==============================================================================
// Private helper methods
//==============================================================================

jsi_pointer *JsiAbiRuntime::getABIPointer(const PointerValue *pv) const {
  return static_cast<const ManagedPointerHolder *>(pv)->pointer();
}

jsi_object JsiAbiRuntime::toABIObject(
    const facebook::jsi::Object &obj) const {
  return {getABIPointer(getPointerValue(obj))};
}
jsi_string JsiAbiRuntime::toABIString(
    const facebook::jsi::String &str) const {
  return {getABIPointer(getPointerValue(str))};
}
jsi_symbol JsiAbiRuntime::toABISymbol(
    const facebook::jsi::Symbol &sym) const {
  return {getABIPointer(getPointerValue(sym))};
}
jsi_propnameid JsiAbiRuntime::toABIPropNameID(
    const facebook::jsi::PropNameID &name) const {
  return {getABIPointer(getPointerValue(name))};
}
jsi_function JsiAbiRuntime::toABIFunction(
    const facebook::jsi::Function &fn) const {
  return {getABIPointer(getPointerValue(fn))};
}
jsi_array JsiAbiRuntime::toABIArray(
    const facebook::jsi::Array &arr) const {
  return {getABIPointer(getPointerValue(arr))};
}
jsi_arraybuffer JsiAbiRuntime::toABIArrayBuffer(
    const facebook::jsi::ArrayBuffer &ab) const {
  return {getABIPointer(getPointerValue(ab))};
}
jsi_weak_object JsiAbiRuntime::toABIWeakObject(
    const facebook::jsi::WeakObject &wo) const {
  return {getABIPointer(getPointerValue(wo))};
}
#if JSI_VERSION >= 6
jsi_bigint JsiAbiRuntime::toABIBigInt(
    const facebook::jsi::BigInt &bi) const {
  return {getABIPointer(getPointerValue(bi))};
}
#endif

jsi_value JsiAbiRuntime::toABIValue(
    const facebook::jsi::Value &val) const {
  if (val.isUndefined())
    return abi::create_undefined_value();
  if (val.isNull())
    return abi::create_null_value();
  if (val.isBool())
    return abi::create_bool_value(val.getBool());
  if (val.isNumber())
    return abi::create_number_value(val.getNumber());
  auto *pv = getPointerValue(val);
  if (pv) {
    auto *mp = getABIPointer(pv);
    if (val.isString())
      return abi::create_string_value(mp);
    if (val.isObject())
      return abi::create_object_value(mp);
    if (val.isSymbol())
      return abi::create_symbol_value(mp);
#if JSI_VERSION >= 6
    if (val.isBigInt())
      return abi::create_bigint_value(mp);
#endif
  }
  return abi::create_undefined_value();
}

jsi_value JsiAbiRuntime::cloneToABIValue(
    const facebook::jsi::Value &val) const {
  if (val.isUndefined())
    return abi::create_undefined_value();
  if (val.isNull())
    return abi::create_null_value();
  if (val.isBool())
    return abi::create_bool_value(val.getBool());
  if (val.isNumber())
    return abi::create_number_value(val.getNumber());
  auto *pv = getPointerValue(val);
  if (pv) {
    auto *mp = getABIPointer(pv);
    if (val.isString())
      return abi::create_string_value(
          api_->clone_string(abiRt_, {mp}).pointer);
    if (val.isObject())
      return abi::create_object_value(
          api_->clone_object(abiRt_, {mp}).pointer);
    if (val.isSymbol())
      return abi::create_symbol_value(
          api_->clone_symbol(abiRt_, {mp}).pointer);
#if JSI_VERSION >= 6
    if (val.isBigInt())
      return abi::create_bigint_value(
          api_->clone_bigint(abiRt_, {mp}).pointer);
#endif
  }
  return abi::create_undefined_value();
}

facebook::jsi::Value JsiAbiRuntime::cloneToJSIValue(const jsi_value &val) {
  switch (abi::get_value_kind(val)) {
    case jsi_valuekind_undefined:
      return facebook::jsi::Value::undefined();
    case jsi_valuekind_null:
      return facebook::jsi::Value::null();
    case jsi_valuekind_boolean:
      return facebook::jsi::Value(abi::get_bool_value(val));
    case jsi_valuekind_number:
      return facebook::jsi::Value(abi::get_number_value(val));
    case jsi_valuekind_string: {
      auto cloned = api_->clone_string(abiRt_, {val.data.pointer});
      return facebook::jsi::Value(make<facebook::jsi::String>(
          new ManagedPointerHolder(cloned.pointer)));
    }
    case jsi_valuekind_object: {
      auto cloned = api_->clone_object(abiRt_, {val.data.pointer});
      return facebook::jsi::Value(make<facebook::jsi::Object>(
          new ManagedPointerHolder(cloned.pointer)));
    }
    case jsi_valuekind_symbol: {
      auto cloned = api_->clone_symbol(abiRt_, {val.data.pointer});
      return facebook::jsi::Value(make<facebook::jsi::Symbol>(
          new ManagedPointerHolder(cloned.pointer)));
    }
    case jsi_valuekind_bigint: {
      auto cloned = api_->clone_bigint(abiRt_, {val.data.pointer});
      return facebook::jsi::Value(make<facebook::jsi::BigInt>(
          new ManagedPointerHolder(cloned.pointer)));
    }
    default:
      return facebook::jsi::Value::undefined();
  }
}

facebook::jsi::Value JsiAbiRuntime::intoJSIValue(jsi_value val) {
  switch (abi::get_value_kind(val)) {
    case jsi_valuekind_undefined:
      return facebook::jsi::Value::undefined();
    case jsi_valuekind_null:
      return facebook::jsi::Value::null();
    case jsi_valuekind_boolean:
      return facebook::jsi::Value(abi::get_bool_value(val));
    case jsi_valuekind_number:
      return facebook::jsi::Value(abi::get_number_value(val));
    case jsi_valuekind_string:
      return facebook::jsi::Value(make<facebook::jsi::String>(
          new ManagedPointerHolder(val.data.pointer)));
    case jsi_valuekind_object:
      return facebook::jsi::Value(make<facebook::jsi::Object>(
          new ManagedPointerHolder(val.data.pointer)));
    case jsi_valuekind_symbol:
      return facebook::jsi::Value(make<facebook::jsi::Symbol>(
          new ManagedPointerHolder(val.data.pointer)));
    case jsi_valuekind_bigint:
      return facebook::jsi::Value(make<facebook::jsi::BigInt>(
          new ManagedPointerHolder(val.data.pointer)));
    default:
      abi::release_value(val);
      return facebook::jsi::Value::undefined();
  }
}

//==============================================================================
// Error handling
//==============================================================================

void JsiAbiRuntime::throwError(jsi_status err) {
  if ((err & 0xF000) == 0x4000) {
    jsi_value jsErr = api_->get_and_clear_js_error_value(abiRt_);
    facebook::jsi::Value errVal = intoJSIValue(jsErr);

    if (activeJSError_)
      throw facebook::jsi::JSINativeException(
          "Error thrown while handling error.");

    SaveAndRestore<bool> s(activeJSError_);
    activeJSError_ = true;
    throw facebook::jsi::JSError(*this, std::move(errVal));
  }
  StringByteBuffer gb;
  api_->get_and_clear_native_exception_message(abiRt_, &gb);
  std::string message = std::move(gb).get();
  if (message.empty()) {
    char fallback[48];
    std::snprintf(
        fallback,
        sizeof(fallback),
        "JSI ABI call failed with status 0x%04X",
        static_cast<unsigned int>(err));
    message = fallback;
  }
  throw facebook::jsi::JSINativeException(std::move(message));
}

void JsiAbiRuntime::checkStatus(jsi_status err) {
  if (err != jsi_status_ok)
    throwError(err);
}

template <typename Fn>
jsi_status JsiAbiRuntime::abiRethrow(Fn fn) {
  try {
    return fn();
  } catch (const facebook::jsi::JSError &e) {
    jsi_value errVal = cloneToABIValue(e.value());
    api_->set_js_error_value(abiRt_, &errVal);
    abi::release_value(errVal);
    return jsi_status_js_error;
  } catch (const std::exception &e) {
    auto *msg = e.what();
    api_->set_native_exception_message_utf8(
        abiRt_, msg, strlen(msg));
    return jsi_status_native_error;
  } catch (...) {
    const char *msg = "Unknown exception in host callback";
    api_->set_native_exception_message_utf8(
        abiRt_, msg, strlen(msg));
    return jsi_status_native_error;
  }
}

facebook::jsi::PropNameID JsiAbiRuntime::cloneToJSIPropNameID(
    jsi_propnameid name) {
  jsi_propnameid cloned = api_->clone_propnameid(abiRt_, name);
  return make<facebook::jsi::PropNameID>(
      new ManagedPointerHolder(cloned.pointer));
}

//==============================================================================
// Constructor / Destructor
//==============================================================================

JsiAbiRuntime::JsiAbiRuntime(jsi_runtime *abiRuntime)
    : api_(abiRuntime->api), abiRt_(abiRuntime) {}

JsiAbiRuntime::~JsiAbiRuntime() {
  api_->release(abiRt_);
}

//==============================================================================
// Script evaluation
//==============================================================================

facebook::jsi::Value JsiAbiRuntime::evaluateJavaScript(
    const std::shared_ptr<const facebook::jsi::Buffer> &buffer,
    const std::string &sourceURL) {
  auto *abiBuf = new BufferWrapper(buffer);
  jsi_value result = abi::create_undefined_value();
  checkStatus(api_->evaluate_javascript_source(
      abiRt_, abiBuf, sourceURL.c_str(), sourceURL.size(), &result));
  return intoJSIValue(result);
}

std::shared_ptr<const facebook::jsi::PreparedJavaScript>
JsiAbiRuntime::prepareJavaScript(
    const std::shared_ptr<const facebook::jsi::Buffer> &buffer,
    std::string sourceURL) {
  auto *abiBuf = new BufferWrapper(buffer);
  jsi_prepared_javascript *prepared = nullptr;
  checkStatus(api_->prepare_javascript(
      abiRt_, abiBuf, sourceURL.c_str(), sourceURL.size(), &prepared));
  auto wrapper = std::make_shared<PreparedJSWrapper>();
  wrapper->prepared = prepared;
  wrapper->api = api_;
  wrapper->rt = abiRt_;
  return wrapper;
}

facebook::jsi::Value JsiAbiRuntime::evaluatePreparedJavaScript(
    const std::shared_ptr<const facebook::jsi::PreparedJavaScript> &js) {
  auto *wrapper = static_cast<const PreparedJSWrapper *>(js.get());
  // Pass the prepared handle by pointer (borrow); the wrapper retains
  // ownership and releases on destruction.
  jsi_value result = abi::create_undefined_value();
  checkStatus(api_->evaluate_prepared_javascript(
      abiRt_, wrapper->prepared, &result));
  return intoJSIValue(result);
}

//==============================================================================
// Microtasks
//==============================================================================

#if JSI_VERSION >= 4
bool JsiAbiRuntime::drainMicrotasks(int maxMicrotasksHint) {
  bool drained = false;
  checkStatus(
      api_->drain_microtasks(abiRt_, maxMicrotasksHint, &drained));
  return drained;
}
#endif

#if JSI_VERSION >= 12
void JsiAbiRuntime::queueMicrotask(
    const facebook::jsi::Function &callback) {
  checkStatus(api_->queue_microtask(abiRt_, toABIFunction(callback)));
}
#endif

//==============================================================================
// Global / Description / Inspectable
//==============================================================================

facebook::jsi::Object JsiAbiRuntime::global() {
  auto obj = api_->get_global_object(abiRt_);
  return make<facebook::jsi::Object>(
      new ManagedPointerHolder(obj.pointer));
}

std::string JsiAbiRuntime::description() {
  StringByteBuffer gb;
  api_->get_description(abiRt_, &gb);
  return std::move(gb).get();
}

bool JsiAbiRuntime::isInspectable() {
  return api_->is_inspectable(abiRt_);
}

//==============================================================================
// Clone operations
//==============================================================================

JsiAbiRuntime::PointerValue *JsiAbiRuntime::cloneSymbol(
    const PointerValue *pv) {
  auto sym = api_->clone_symbol(abiRt_, {getABIPointer(pv)});
  return new ManagedPointerHolder(sym.pointer);
}

JsiAbiRuntime::PointerValue *JsiAbiRuntime::cloneString(
    const PointerValue *pv) {
  auto str = api_->clone_string(abiRt_, {getABIPointer(pv)});
  return new ManagedPointerHolder(str.pointer);
}

#if JSI_VERSION >= 6
JsiAbiRuntime::PointerValue *JsiAbiRuntime::cloneBigInt(
    const PointerValue *pv) {
  auto bi = api_->clone_bigint(abiRt_, {getABIPointer(pv)});
  return new ManagedPointerHolder(bi.pointer);
}
#endif

JsiAbiRuntime::PointerValue *JsiAbiRuntime::cloneObject(
    const PointerValue *pv) {
  auto obj = api_->clone_object(abiRt_, {getABIPointer(pv)});
  return new ManagedPointerHolder(obj.pointer);
}

JsiAbiRuntime::PointerValue *JsiAbiRuntime::clonePropNameID(
    const PointerValue *pv) {
  auto name = api_->clone_propnameid(abiRt_, {getABIPointer(pv)});
  return new ManagedPointerHolder(name.pointer);
}

//==============================================================================
// PropNameID operations
//==============================================================================

facebook::jsi::PropNameID JsiAbiRuntime::createPropNameIDFromAscii(
    const char *str,
    size_t length) {
  return createPropNameIDFromUtf8(
      reinterpret_cast<const uint8_t *>(str), length);
}

facebook::jsi::PropNameID JsiAbiRuntime::createPropNameIDFromUtf8(
    const uint8_t *utf8,
    size_t length) {
  jsi_propnameid result{};
  checkStatus(api_->create_propnameid_utf8(
      abiRt_, reinterpret_cast<const char *>(utf8), length, &result));
  return make<facebook::jsi::PropNameID>(
      new ManagedPointerHolder(result.pointer));
}

facebook::jsi::PropNameID JsiAbiRuntime::createPropNameIDFromString(
    const facebook::jsi::String &str) {
  jsi_propnameid result{};
  checkStatus(api_->create_propnameid_from_string(
      abiRt_, toABIString(str), &result));
  return make<facebook::jsi::PropNameID>(
      new ManagedPointerHolder(result.pointer));
}

#if JSI_VERSION >= 5
facebook::jsi::PropNameID JsiAbiRuntime::createPropNameIDFromSymbol(
    const facebook::jsi::Symbol &sym) {
  jsi_propnameid result{};
  checkStatus(api_->create_propnameid_from_symbol(
      abiRt_, toABISymbol(sym), &result));
  return make<facebook::jsi::PropNameID>(
      new ManagedPointerHolder(result.pointer));
}
#endif

std::string JsiAbiRuntime::utf8(const facebook::jsi::PropNameID &name) {
  StringByteBuffer gb;
  api_->get_propnameid_utf8(abiRt_, toABIPropNameID(name), &gb);
  return std::move(gb).get();
}

bool JsiAbiRuntime::compare(
    const facebook::jsi::PropNameID &a,
    const facebook::jsi::PropNameID &b) {
  return api_->propnameid_equals(
      abiRt_, toABIPropNameID(a), toABIPropNameID(b));
}

//==============================================================================
// Symbol operations
//==============================================================================

std::string JsiAbiRuntime::symbolToString(
    const facebook::jsi::Symbol &sym) {
  StringByteBuffer gb;
  api_->get_symbol_utf8(abiRt_, toABISymbol(sym), &gb);
  return std::move(gb).get();
}

//==============================================================================
// BigInt operations
//==============================================================================

#if JSI_VERSION >= 8
facebook::jsi::BigInt JsiAbiRuntime::createBigIntFromInt64(int64_t val) {
  jsi_bigint result{};
  checkStatus(api_->create_bigint_from_int64(abiRt_, val, &result));
  return make<facebook::jsi::BigInt>(
      new ManagedPointerHolder(result.pointer));
}

facebook::jsi::BigInt JsiAbiRuntime::createBigIntFromUint64(uint64_t val) {
  jsi_bigint result{};
  checkStatus(api_->create_bigint_from_uint64(abiRt_, val, &result));
  return make<facebook::jsi::BigInt>(
      new ManagedPointerHolder(result.pointer));
}

bool JsiAbiRuntime::bigintIsInt64(const facebook::jsi::BigInt &bi) {
  return api_->bigint_is_int64(abiRt_, toABIBigInt(bi));
}

bool JsiAbiRuntime::bigintIsUint64(const facebook::jsi::BigInt &bi) {
  return api_->bigint_is_uint64(abiRt_, toABIBigInt(bi));
}

uint64_t JsiAbiRuntime::truncate(const facebook::jsi::BigInt &bi) {
  return api_->bigint_truncate_to_uint64(abiRt_, toABIBigInt(bi));
}

facebook::jsi::String JsiAbiRuntime::bigintToString(
    const facebook::jsi::BigInt &bi,
    int radix) {
  jsi_string result{};
  checkStatus(api_->bigint_to_string(
      abiRt_, toABIBigInt(bi), static_cast<uint32_t>(radix), &result));
  return make<facebook::jsi::String>(
      new ManagedPointerHolder(result.pointer));
}
#endif

//==============================================================================
// String operations
//==============================================================================

facebook::jsi::String JsiAbiRuntime::createStringFromAscii(
    const char *str,
    size_t length) {
  return createStringFromUtf8(
      reinterpret_cast<const uint8_t *>(str), length);
}

facebook::jsi::String JsiAbiRuntime::createStringFromUtf8(
    const uint8_t *utf8,
    size_t length) {
  jsi_string result{};
  checkStatus(api_->create_string_utf8(
      abiRt_, reinterpret_cast<const char *>(utf8), length, &result));
  return make<facebook::jsi::String>(
      new ManagedPointerHolder(result.pointer));
}

std::string JsiAbiRuntime::utf8(const facebook::jsi::String &str) {
  StringByteBuffer gb;
  api_->get_string_utf8(abiRt_, toABIString(str), &gb);
  return std::move(gb).get();
}

//==============================================================================
// Object operations
//==============================================================================

facebook::jsi::Object JsiAbiRuntime::createObject() {
  jsi_object result{};
  checkStatus(api_->create_object(abiRt_, &result));
  return make<facebook::jsi::Object>(
      new ManagedPointerHolder(result.pointer));
}

facebook::jsi::Object JsiAbiRuntime::createObject(
    std::shared_ptr<facebook::jsi::HostObject> ho) {
  auto *wrapper = new HostObjectWrapper(*this, std::move(ho));
  jsi_object result{};
  checkStatus(api_->create_object_from_host_object(
      abiRt_, wrapper, &result));
  return make<facebook::jsi::Object>(
      new ManagedPointerHolder(result.pointer));
}

std::shared_ptr<facebook::jsi::HostObject> JsiAbiRuntime::getHostObject(
    const facebook::jsi::Object &obj) {
  auto *ho = api_->get_host_object(abiRt_, toABIObject(obj));
  if (!ho)
    return nullptr;
  if (ho->api == HostObjectWrapper::getVt())
    return static_cast<HostObjectWrapper *>(ho)->getHostObject();
  return nullptr;
}

facebook::jsi::HostFunctionType &JsiAbiRuntime::getHostFunction(
    const facebook::jsi::Function &fn) {
  auto *hf = api_->get_host_function(abiRt_, toABIFunction(fn));
  assert(hf && "Not a host function");
  return static_cast<HostFunctionWrapper *>(hf)->getHostFunction();
}

#if JSI_VERSION >= 7
bool JsiAbiRuntime::hasNativeState(const facebook::jsi::Object &obj) {
  return api_->has_native_state(abiRt_, toABIObject(obj));
}

std::shared_ptr<facebook::jsi::NativeState>
JsiAbiRuntime::getNativeState(const facebook::jsi::Object &obj) {
  auto *ns = api_->get_native_state(abiRt_, toABIObject(obj));
  if (!ns)
    return nullptr;
  if (ns->api == NativeStateWrapper::getVt())
    return static_cast<NativeStateWrapper *>(ns)->getNativeState();
  return nullptr;
}

void JsiAbiRuntime::setNativeState(
    const facebook::jsi::Object &obj,
    std::shared_ptr<facebook::jsi::NativeState> state) {
  auto *wrapper = new NativeStateWrapper(std::move(state));
  checkStatus(api_->set_native_state(
      abiRt_, toABIObject(obj), wrapper));
}
#endif

//==============================================================================
// Prototype operations
//==============================================================================

#if JSI_VERSION >= 17
void JsiAbiRuntime::setPrototypeOf(
    const facebook::jsi::Object &object,
    const facebook::jsi::Value &prototype) {
  jsi_value proto = toABIValue(prototype);
  checkStatus(
      api_->set_prototype_of(abiRt_, toABIObject(object), &proto));
}

facebook::jsi::Value JsiAbiRuntime::getPrototypeOf(
    const facebook::jsi::Object &object) {
  jsi_value result = abi::create_undefined_value();
  checkStatus(
      api_->get_prototype_of(abiRt_, toABIObject(object), &result));
  return intoJSIValue(result);
}
#endif

//==============================================================================
// Property operations
//==============================================================================

facebook::jsi::Value JsiAbiRuntime::getProperty(
    const facebook::jsi::Object &obj,
    const facebook::jsi::PropNameID &name) {
  jsi_value result = abi::create_undefined_value();
  checkStatus(api_->get_object_property_from_propnameid(
      abiRt_, toABIObject(obj), toABIPropNameID(name), &result));
  return intoJSIValue(result);
}

facebook::jsi::Value JsiAbiRuntime::getProperty(
    const facebook::jsi::Object &obj,
    const facebook::jsi::String &name) {
  jsi_value key =
      abi::create_string_value(getABIPointer(getPointerValue(name)));
  jsi_value result = abi::create_undefined_value();
  checkStatus(api_->get_object_property_from_value(
      abiRt_, toABIObject(obj), &key, &result));
  return intoJSIValue(result);
}

bool JsiAbiRuntime::hasProperty(
    const facebook::jsi::Object &obj,
    const facebook::jsi::PropNameID &name) {
  bool result = false;
  checkStatus(api_->has_object_property_from_propnameid(
      abiRt_, toABIObject(obj), toABIPropNameID(name), &result));
  return result;
}

bool JsiAbiRuntime::hasProperty(
    const facebook::jsi::Object &obj,
    const facebook::jsi::String &name) {
  jsi_value key =
      abi::create_string_value(getABIPointer(getPointerValue(name)));
  bool result = false;
  checkStatus(api_->has_object_property_from_value(
      abiRt_, toABIObject(obj), &key, &result));
  return result;
}

void JsiAbiRuntime::setPropertyValue(
    JSI_CONST_10 facebook::jsi::Object &obj,
    const facebook::jsi::PropNameID &name,
    const facebook::jsi::Value &value) {
  jsi_value val = toABIValue(value);
  checkStatus(api_->set_object_property_from_propnameid(
      abiRt_, toABIObject(obj), toABIPropNameID(name), &val));
}

void JsiAbiRuntime::setPropertyValue(
    JSI_CONST_10 facebook::jsi::Object &obj,
    const facebook::jsi::String &name,
    const facebook::jsi::Value &value) {
  jsi_value key =
      abi::create_string_value(getABIPointer(getPointerValue(name)));
  jsi_value val = toABIValue(value);
  checkStatus(api_->set_object_property_from_value(
      abiRt_, toABIObject(obj), &key, &val));
}

//==============================================================================
// Type checks
//==============================================================================

bool JsiAbiRuntime::isArray(const facebook::jsi::Object &obj) const {
  return api_->object_is_array(abiRt_, toABIObject(obj));
}

bool JsiAbiRuntime::isArrayBuffer(
    const facebook::jsi::Object &obj) const {
  return api_->object_is_arraybuffer(abiRt_, toABIObject(obj));
}

bool JsiAbiRuntime::isFunction(const facebook::jsi::Object &obj) const {
  return api_->object_is_function(abiRt_, toABIObject(obj));
}

bool JsiAbiRuntime::isHostObject(
    const facebook::jsi::Object &obj) const {
  auto *ho = api_->get_host_object(abiRt_, toABIObject(obj));
  return ho && ho->api == HostObjectWrapper::getVt();
}

bool JsiAbiRuntime::isHostFunction(
    const facebook::jsi::Function &fn) const {
  auto *hf = api_->get_host_function(abiRt_, toABIFunction(fn));
  return hf && hf->api == HostFunctionWrapper::getVt();
}

facebook::jsi::Array JsiAbiRuntime::getPropertyNames(
    const facebook::jsi::Object &obj) {
  jsi_array result{};
  checkStatus(
      api_->get_object_property_names(abiRt_, toABIObject(obj), &result));
  return make<facebook::jsi::Object>(
             new ManagedPointerHolder(result.pointer))
      .getArray(*this);
}

//==============================================================================
// Weak references
//==============================================================================

facebook::jsi::WeakObject JsiAbiRuntime::createWeakObject(
    const facebook::jsi::Object &obj) {
  jsi_weak_object result{};
  checkStatus(api_->create_weak_object(
      abiRt_, toABIObject(obj), &result));
  return make<facebook::jsi::WeakObject>(
      new ManagedPointerHolder(result.pointer));
}

facebook::jsi::Value JsiAbiRuntime::lockWeakObject(
    JSI_NO_CONST_3 JSI_CONST_10 facebook::jsi::WeakObject &wo) {
  auto val = api_->lock_weak_object(abiRt_, toABIWeakObject(wo));
  return intoJSIValue(val);
}

//==============================================================================
// Array operations
//==============================================================================

facebook::jsi::Array JsiAbiRuntime::createArray(size_t length) {
  jsi_array result{};
  checkStatus(api_->create_array(abiRt_, length, &result));
  return make<facebook::jsi::Object>(
             new ManagedPointerHolder(result.pointer))
      .getArray(*this);
}

#if JSI_VERSION >= 9
facebook::jsi::ArrayBuffer JsiAbiRuntime::createArrayBuffer(
    std::shared_ptr<facebook::jsi::MutableBuffer> buffer) {
  auto *abiBuf = new MutableBufferWrapper(std::move(buffer));
  jsi_arraybuffer result{};
  checkStatus(api_->create_arraybuffer_from_external_data(
      abiRt_, abiBuf, &result));
  return make<facebook::jsi::Object>(
             new ManagedPointerHolder(result.pointer))
      .getArrayBuffer(*this);
}
#endif

size_t JsiAbiRuntime::size(const facebook::jsi::Array &arr) {
  return api_->get_array_length(abiRt_, toABIArray(arr));
}

size_t JsiAbiRuntime::size(const facebook::jsi::ArrayBuffer &ab) {
  size_t result = 0;
  checkStatus(api_->get_arraybuffer_size(
      abiRt_, toABIArrayBuffer(ab), &result));
  return result;
}

uint8_t *JsiAbiRuntime::data(const facebook::jsi::ArrayBuffer &ab) {
  uint8_t *result = nullptr;
  checkStatus(api_->get_arraybuffer_data(
      abiRt_, toABIArrayBuffer(ab), &result));
  return result;
}

facebook::jsi::Value JsiAbiRuntime::getValueAtIndex(
    const facebook::jsi::Array &arr,
    size_t i) {
  jsi_value result = abi::create_undefined_value();
  checkStatus(
      api_->get_array_element(abiRt_, toABIObject(arr), i, &result));
  return intoJSIValue(result);
}

void JsiAbiRuntime::setValueAtIndexImpl(
    JSI_CONST_10 facebook::jsi::Array &arr,
    size_t i,
    const facebook::jsi::Value &value) {
  jsi_value val = toABIValue(value);
  checkStatus(
      api_->set_array_element(abiRt_, toABIObject(arr), i, &val));
}

//==============================================================================
// Function operations
//==============================================================================

facebook::jsi::Function JsiAbiRuntime::createFunctionFromHostFunction(
    const facebook::jsi::PropNameID &name,
    unsigned int paramCount,
    facebook::jsi::HostFunctionType func) {
  auto *wrapper = new HostFunctionWrapper(*this, std::move(func));
  jsi_function result{};
  checkStatus(api_->create_function_from_host_function(
      abiRt_, toABIPropNameID(name), paramCount, wrapper, &result));
  return make<facebook::jsi::Object>(
             new ManagedPointerHolder(result.pointer))
      .getFunction(*this);
}

facebook::jsi::Value JsiAbiRuntime::call(
    const facebook::jsi::Function &fn,
    const facebook::jsi::Value &jsThis,
    const facebook::jsi::Value *args,
    size_t count) {
  std::vector<jsi_value> abiArgs(count);
  for (size_t i = 0; i < count; ++i)
    abiArgs[i] = toABIValue(args[i]);
  jsi_value abiThis = toABIValue(jsThis);
  jsi_value result = abi::create_undefined_value();
  checkStatus(api_->call(
      abiRt_,
      toABIFunction(fn),
      &abiThis,
      abiArgs.data(),
      count,
      &result));
  return intoJSIValue(result);
}

facebook::jsi::Value JsiAbiRuntime::callAsConstructor(
    const facebook::jsi::Function &fn,
    const facebook::jsi::Value *args,
    size_t count) {
  std::vector<jsi_value> abiArgs(count);
  for (size_t i = 0; i < count; ++i)
    abiArgs[i] = toABIValue(args[i]);
  jsi_value result = abi::create_undefined_value();
  checkStatus(api_->call_as_constructor(
      abiRt_, toABIFunction(fn), abiArgs.data(), count, &result));
  return intoJSIValue(result);
}

//==============================================================================
// Comparison
//==============================================================================

bool JsiAbiRuntime::strictEquals(
    const facebook::jsi::Symbol &a,
    const facebook::jsi::Symbol &b) const {
  return api_->strict_equals_symbol(
      abiRt_, toABISymbol(a), toABISymbol(b));
}

#if JSI_VERSION >= 6
bool JsiAbiRuntime::strictEquals(
    const facebook::jsi::BigInt &a,
    const facebook::jsi::BigInt &b) const {
  return api_->strict_equals_bigint(
      abiRt_, toABIBigInt(a), toABIBigInt(b));
}
#endif

bool JsiAbiRuntime::strictEquals(
    const facebook::jsi::String &a,
    const facebook::jsi::String &b) const {
  return api_->strict_equals_string(
      abiRt_, toABIString(a), toABIString(b));
}

bool JsiAbiRuntime::strictEquals(
    const facebook::jsi::Object &a,
    const facebook::jsi::Object &b) const {
  return api_->strict_equals_object(
      abiRt_, toABIObject(a), toABIObject(b));
}

bool JsiAbiRuntime::instanceOf(
    const facebook::jsi::Object &o,
    const facebook::jsi::Function &f) {
  bool result = false;
  checkStatus(api_->instance_of(
      abiRt_, toABIObject(o), toABIFunction(f), &result));
  return result;
}

//==============================================================================
// External memory pressure
//==============================================================================

#if JSI_VERSION >= 11
void JsiAbiRuntime::setExternalMemoryPressure(
    const facebook::jsi::Object &obj,
    size_t amount) {
  checkStatus(api_->set_object_external_memory_pressure(
      abiRt_, toABIObject(obj), amount));
}
#endif

//==============================================================================
// Scopes
//==============================================================================

facebook::jsi::Runtime::ScopeState *JsiAbiRuntime::pushScope() {
  void *scope = nullptr;
  checkStatus(api_->push_scope(abiRt_, &scope));
  return reinterpret_cast<ScopeState *>(scope);
}

void JsiAbiRuntime::popScope(ScopeState *state) {
  checkStatus(
      api_->pop_scope(abiRt_, reinterpret_cast<void *>(state)));
}

//==============================================================================
// Factory functions (exported via JsiAbiRuntime.h)
//==============================================================================

std::unique_ptr<facebook::jsi::Runtime> makeJsiAbiRuntime(
    jsi_create_runtime_fn create_runtime,
    jsi_configure_runtime_cb configure,
    void *user_data) {
  // Inject the consumer's compiled JSI_ABI_VERSION (Node-API Model B). The
  // create function hands back a runtime with refcount 1; the wrapper adopts
  // that ref (destructor releases).
  jsi_runtime *rt = nullptr;
  jsi_status status =
      create_runtime(JSI_ABI_VERSION, configure, user_data, &rt);
  if (status != jsi_status_ok || !rt) {
    char message[64];
    std::snprintf(
        message,
        sizeof(message),
        "JSI ABI runtime creation failed with status 0x%04X",
        static_cast<unsigned int>(status));
    throw facebook::jsi::JSINativeException(message);
  }
  return std::make_unique<JsiAbiRuntime>(rt);
}

std::unique_ptr<facebook::jsi::Runtime> wrapJsiRuntime(
    jsi_runtime *abiRuntime) {
  // Add a ref on entry; the wrapper adopts that fresh ref. The caller's
  // ref (if any) is independent.
  abiRuntime->api->add_ref(abiRuntime);
  return std::make_unique<JsiAbiRuntime>(abiRuntime);
}

jsi_runtime *getAbiRuntime(facebook::jsi::Runtime &runtime) noexcept {
  // Caller asserts the supplied Runtime was created via the factories above.
  return static_cast<JsiAbiRuntime &>(runtime).abiRuntime();
}

} // namespace jsi::abi
