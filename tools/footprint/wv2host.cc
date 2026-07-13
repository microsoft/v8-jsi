// wv2host.exe — minimal real WebView2 host for the footprint comparison
// (measurement-only; not shipped). Creates N CoreWebView2 controls sharing ONE
// environment / user-data-folder, navigates each to the SAME JS workload the
// sandbox v8host runs, and — once all N have signalled completion — holds so the
// harness can sample the whole msedgewebview2.exe process tree, then exits.
//
// Driving both N-controls-in-one-host (shared browser process) and N-separate-
// hosts (isolated, distinct UDF per host) is done from the harness by choosing
// --controls / --udf. This is the WebView2 control Office finds too heavy; the
// point of this harness is to quantify exactly how much heavier than the sandbox.
//
// Build: links WebView2LoaderStatic.lib (Evergreen runtime auto-detected).
//
// Args:
//   --controls N         number of CoreWebView2 controls in THIS host (default 1)
//   --udf <path>         user-data-folder (default: a temp subdir)
//   --hold-ms M          hold after all controls ready, in ms (default 10000)
//   --release-event NAME manual-reset event; set by harness to release early

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <wrl.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "WebView2.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "version.lib")

using namespace Microsoft::WRL;

namespace {

int g_controls_wanted = 1;
int g_controls_ready = 0;
DWORD g_hold_ms = 10000;
std::wstring g_release_event;
HWND g_hwnd = nullptr;
ICoreWebView2Environment* g_env = nullptr;

// Same compute the sandbox v8host runs (kUserJs compute loop), plus a touch of
// string work, then signal completion to the host. Identical work => apples to
// apples on the JS; the rest of WebView2's footprint is the comparison's point.
const wchar_t* kWorkloadHtml =
    L"<!doctype html><html><head><meta charset='utf-8'></head><body>"
    L"<script>"
    L"(function(){var s=0;for(var i=0;i<3000000;i++){s+=i;}return s;})();"
    L"var m='ready';var r='js echo: '+m;void r;"
    L"window.chrome.webview.postMessage('done');"
    L"</script></body></html>";

void StartHoldThenQuit() {
  printf("[wv2host] READY controls=%d (all loaded) hold_ms=%lu\n",
         g_controls_ready, g_hold_ms);
  fflush(stdout);
  HANDLE rel = nullptr;
  if (!g_release_event.empty())
    rel = ::OpenEventW(SYNCHRONIZE, FALSE, g_release_event.c_str());
  if (rel) {
    ::WaitForSingleObject(rel, g_hold_ms);
    ::CloseHandle(rel);
  } else {
    ::Sleep(g_hold_ms);
  }
  printf("[wv2host] RELEASE -> quitting\n");
  fflush(stdout);
  ::PostQuitMessage(0);
}

HRESULT OnWebMessage(ICoreWebView2* /*sender*/,
                     ICoreWebView2WebMessageReceivedEventArgs* args) {
  LPWSTR msg = nullptr;
  if (SUCCEEDED(args->TryGetWebMessageAsString(&msg)) && msg) {
    if (wcscmp(msg, L"done") == 0) {
      ++g_controls_ready;
      printf("[wv2host] control %d/%d done\n", g_controls_ready,
             g_controls_wanted);
      fflush(stdout);
      if (g_controls_ready >= g_controls_wanted)
        StartHoldThenQuit();
    }
    ::CoTaskMemFree(msg);
  }
  return S_OK;
}

HRESULT OnControllerCreated(HRESULT result, ICoreWebView2Controller* controller) {
  if (FAILED(result) || !controller) {
    printf("[wv2host] controller create failed hr=0x%08lx\n", result);
    return result;
  }
  controller->put_IsVisible(FALSE);
  ComPtr<ICoreWebView2> webview;
  controller->get_CoreWebView2(&webview);
  if (!webview)
    return E_FAIL;
  EventRegistrationToken tok = {};
  webview->add_WebMessageReceived(
      Callback<ICoreWebView2WebMessageReceivedEventHandler>(
          [](ICoreWebView2* s,
             ICoreWebView2WebMessageReceivedEventArgs* a) -> HRESULT {
            return OnWebMessage(s, a);
          })
          .Get(),
      &tok);
  webview->NavigateToString(kWorkloadHtml);
  // Keep controller/webview alive for the process lifetime (held by COM through
  // the environment + the message loop); intentionally leak the ref here.
  controller->AddRef();
  return S_OK;
}

HRESULT OnEnvCreated(HRESULT result, ICoreWebView2Environment* env) {
  if (FAILED(result) || !env) {
    printf("[wv2host] env create failed hr=0x%08lx\n", result);
    ::PostQuitMessage(1);
    return result;
  }
  g_env = env;
  env->AddRef();
  for (int i = 0; i < g_controls_wanted; ++i) {
    HRESULT hr = env->CreateCoreWebView2Controller(
        g_hwnd,
        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [](HRESULT r, ICoreWebView2Controller* c) -> HRESULT {
              return OnControllerCreated(r, c);
            })
            .Get());
    if (FAILED(hr))
      printf("[wv2host] CreateController[%d] hr=0x%08lx\n", i, hr);
  }
  return S_OK;
}

std::wstring ArgVal(int argc, wchar_t** argv, const wchar_t* key,
                    const wchar_t* def) {
  for (int i = 1; i + 1 < argc; ++i)
    if (wcscmp(argv[i], key) == 0)
      return argv[i + 1];
  return def ? std::wstring(def) : std::wstring();
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  g_controls_wanted = _wtoi(ArgVal(argc, argv, L"--controls", L"1").c_str());
  if (g_controls_wanted < 1)
    g_controls_wanted = 1;
  g_hold_ms = (DWORD)_wtoi(ArgVal(argc, argv, L"--hold-ms", L"10000").c_str());
  g_release_event = ArgVal(argc, argv, L"--release-event", L"");

  wchar_t tmp[MAX_PATH] = {};
  ::GetTempPathW(MAX_PATH, tmp);
  std::wstring udf = ArgVal(argc, argv, L"--udf",
                            (std::wstring(tmp) + L"wv2host_udf").c_str());

  printf("[wv2host] pid=%lu controls=%d udf=%ls\n", ::GetCurrentProcessId(),
         g_controls_wanted, udf.c_str());

  HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(hr)) {
    printf("[wv2host] CoInitializeEx failed 0x%08lx\n", hr);
    return 2;
  }

  // Hidden top-level window to parent the WebView2 controllers.
  WNDCLASSW wc = {};
  wc.lpfnWndProc = ::DefWindowProcW;
  wc.hInstance = ::GetModuleHandleW(nullptr);
  wc.lpszClassName = L"wv2host_wnd";
  ::RegisterClassW(&wc);
  g_hwnd = ::CreateWindowExW(0, wc.lpszClassName, L"wv2host", WS_OVERLAPPEDWINDOW,
                             0, 0, 320, 240, nullptr, nullptr, wc.hInstance,
                             nullptr);
  if (!g_hwnd) {
    printf("[wv2host] CreateWindow failed\n");
    return 3;
  }

  hr = ::CreateCoreWebView2EnvironmentWithOptions(
      nullptr, udf.c_str(), nullptr,
      Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
          [](HRESULT r, ICoreWebView2Environment* e) -> HRESULT {
            return OnEnvCreated(r, e);
          })
          .Get());
  if (FAILED(hr)) {
    printf("[wv2host] CreateEnvironment failed 0x%08lx (WebView2 runtime?)\n", hr);
    return 4;
  }

  MSG msg = {};
  while (::GetMessageW(&msg, nullptr, 0, 0)) {
    ::TranslateMessage(&msg);
    ::DispatchMessageW(&msg);
  }
  printf("[wv2host] exit\n");
  return 0;
}
