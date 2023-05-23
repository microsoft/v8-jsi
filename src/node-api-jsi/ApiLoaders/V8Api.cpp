// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "V8Api.h"

namespace Microsoft::NodeApiJsi {

thread_local V8Api *V8Api::current_{};

V8Api::V8Api(IFuncResolver *funcResolver) : JSRuntimeApi(funcResolver) {}

V8Api *V8Api::fromLib() {
  static LibFuncResolver funcResolver("v8jsi");
  static V8Api *libV8Api = new V8Api(&funcResolver);
  return libV8Api;
}

} // namespace Microsoft::NodeApiJsi
