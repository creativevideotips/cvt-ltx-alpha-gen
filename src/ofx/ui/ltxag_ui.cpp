// Windows port: the plugin's own small windows. What mac_progress.swift (the progress window) and
// osascript (messages, the folder picker) are on the Mac, as one program built with the plugin
// and placed beside the .ofx (Contents/Win64/ltxag_ui.exe). Started by ltxag_worker/oslayer.py
// (progress, message) and by the plugin (folder). Arguments and output are UTF-8 / UTF-16 safe.
//
//   ltxag_ui progress <title> <width>
//       A window with a text line, a progress bar and Cancel. Reads zenity's --progress protocol
//       on stdin (UTF-8), as the Mac helper does: "# text" sets the line, a number sets the
//       percentage, "100" or the end of input closes the window (exit 0). Cancel or the close box:
//       exit 1. Cancel never takes the keyboard focus, so Space (play in Resolve and Fusion) can't
//       press it after the window was clicked (cvt1.16).
//   ltxag_ui message info|error <title> <text>
//       A message box with OK.
//   ltxag_ui folder <title> <start folder>
//       Explorer's folder picker; prints the chosen folder (UTF-8) to stdout, nothing on Cancel.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <cstdlib>
#include <string>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' " \
                        "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

constexpr UINT kMsgText = WM_APP + 1;     // lParam: new std::wstring*
constexpr UINT kMsgPercent = WM_APP + 2;  // wParam: 0-100
constexpr UINT kMsgDone = WM_APP + 3;     // end of input or "100"
constexpr int kIdCancel = 100;

HWND gWindow = nullptr, gLabel = nullptr, gBar = nullptr;
int gExit = 0;

std::wstring widen(const std::string& s) {
  if (s.empty()) return std::wstring();
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring out(size_t(n > 0 ? n : 0), L'\0');
  if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), &out[0], n);
  return out;
}

std::string narrow(const std::wstring& s) {
  if (s.empty()) return std::string();
  const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0, nullptr, nullptr);
  std::string out(size_t(n > 0 ? n : 0), '\0');
  if (n > 0) WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), &out[0], n, nullptr, nullptr);
  return out;
}

void handleLine(std::string line) {
  while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
  if (line.empty()) return;
  if (line[0] == '#') {
    size_t at = 1;
    while (at < line.size() && line[at] == ' ') ++at;
    PostMessageW(gWindow, kMsgText, 0, reinterpret_cast<LPARAM>(new std::wstring(widen(line.substr(at)))));
    return;
  }
  char* end = nullptr;
  const long pct = std::strtol(line.c_str(), &end, 10);
  if (end == line.c_str()) return;
  if (pct >= 100)
    PostMessageW(gWindow, kMsgDone, 0, 0);
  else
    PostMessageW(gWindow, kMsgPercent, WPARAM(pct < 0 ? 0 : pct), 0);
}

DWORD WINAPI readInput(LPVOID) {
  const HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
  std::string buf;
  char chunk[1024];
  DWORD got = 0;
  while (in && in != INVALID_HANDLE_VALUE && ReadFile(in, chunk, sizeof chunk, &got, nullptr) && got > 0) {
    buf.append(chunk, got);
    size_t nl;
    while ((nl = buf.find('\n')) != std::string::npos) {
      handleLine(buf.substr(0, nl));
      buf.erase(0, nl + 1);
    }
  }
  PostMessageW(gWindow, kMsgDone, 0, 0);  // the job went away, or it is finished
  return 0;
}

LRESULT CALLBACK windowProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case kMsgText: {
      auto* text = reinterpret_cast<std::wstring*>(lp);
      SetWindowTextW(gLabel, text->c_str());
      delete text;
      return 0;
    }
    case kMsgPercent:
      SendMessageW(gBar, PBM_SETPOS, wp, 0);
      return 0;
    case kMsgDone:
      gExit = 0;
      DestroyWindow(w);
      return 0;
    case WM_COMMAND:
      if (LOWORD(wp) == kIdCancel) {
        gExit = 1;
        DestroyWindow(w);
      }
      return 0;
    case WM_CLOSE:
      gExit = 1;
      DestroyWindow(w);
      return 0;
    case WM_DESTROY:
      PostQuitMessage(gExit);
      return 0;
    case WM_CTLCOLORSTATIC:
      SetBkMode(reinterpret_cast<HDC>(wp), TRANSPARENT);
      return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    default:
      return DefWindowProcW(w, msg, wp, lp);
  }
}

int progress(const std::wstring& title, int width) {
  INITCOMMONCONTROLSEX icc{sizeof icc, ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
  InitCommonControlsEx(&icc);
  const HINSTANCE inst = GetModuleHandleW(nullptr);
  HDC screen = GetDC(nullptr);
  const double scale = GetDeviceCaps(screen, LOGPIXELSX) / 96.0;
  ReleaseDC(nullptr, screen);
  auto px = [scale](int v) { return int(v * scale + 0.5); };

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof wc;
  wc.lpfnWndProc = windowProc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
  wc.lpszClassName = L"LtxagProgress";
  wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
  RegisterClassExW(&wc);

  const int clientW = px(width < 320 ? 320 : width > 1200 ? 1200 : width), clientH = px(128);
  const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
  RECT r{0, 0, clientW, clientH};
  AdjustWindowRectEx(&r, style, FALSE, WS_EX_TOPMOST);
  const int w = r.right - r.left, h = r.bottom - r.top;
  RECT work{};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
  // WS_EX_NOACTIVATE: the window shows without taking the keyboard from the host, where the user
  // may be working while a job runs.
  gWindow = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_APPWINDOW, wc.lpszClassName, title.c_str(), style,
                            work.left + (work.right - work.left - w) / 2, work.top + (work.bottom - work.top - h) / 3,
                            w, h, nullptr, nullptr, inst, nullptr);
  if (!gWindow) return 2;

  NONCLIENTMETRICSW ncm{};
  ncm.cbSize = sizeof ncm;
  SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
  const HFONT font = CreateFontIndirectW(&ncm.lfMessageFont);
  const int m = px(16);
  gLabel = CreateWindowExW(0, L"STATIC", L"Starting", WS_CHILD | WS_VISIBLE | SS_NOPREFIX | SS_EDITCONTROL, m, px(12),
                           clientW - 2 * m, px(38), gWindow, nullptr, inst, nullptr);
  gBar = CreateWindowExW(0, PROGRESS_CLASSW, nullptr, WS_CHILD | WS_VISIBLE | PBS_SMOOTH, m, px(56), clientW - 2 * m,
                         px(18), gWindow, nullptr, inst, nullptr);
  // No WS_TABSTOP, and the window never activates: no key reaches the button.
  const HWND cancel = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, clientW - m - px(88),
                                      px(88), px(88), px(28), gWindow,
                                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdCancel)), inst, nullptr);
  for (HWND c : {gLabel, cancel}) SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
  SendMessageW(gBar, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
  ShowWindow(gWindow, SW_SHOWNOACTIVATE);
  UpdateWindow(gWindow);

  CloseHandle(CreateThread(nullptr, 0, readInput, nullptr, 0, nullptr));
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  return gExit;
}

int message(const std::wstring& kind, const std::wstring& title, const std::wstring& text) {
  MessageBoxW(nullptr, text.c_str(), title.c_str(),
              MB_OK | MB_SETFOREGROUND | MB_TOPMOST | (kind == L"error" ? MB_ICONERROR : MB_ICONINFORMATION));
  return 0;
}

int folder(const std::wstring& title, const std::wstring& start) {
  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) return 2;
  int rc = 1;
  IFileOpenDialog* dialog = nullptr;
  if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->SetTitle(title.c_str());
    IShellItem* startItem = nullptr;
    if (!start.empty() && SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&startItem)))) {
      dialog->SetFolder(startItem);
      startItem->Release();
    }
    if (SUCCEEDED(dialog->Show(nullptr))) {
      IShellItem* item = nullptr;
      if (SUCCEEDED(dialog->GetResult(&item))) {
        wchar_t* path = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
          const std::string out = narrow(path) + "\n";
          DWORD done = 0;
          WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), out.data(), DWORD(out.size()), &done, nullptr);
          CoTaskMemFree(path);
          rc = 0;
        }
        item->Release();
      }
    }
    dialog->Release();
  }
  CoUninitialize();
  return rc;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  SetProcessDPIAware();
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv || argc < 2) return 2;
  const std::wstring mode = argv[1];
  if (mode == L"progress" && argc >= 3) return progress(argv[2], argc >= 4 ? _wtoi(argv[3]) : 520);
  if (mode == L"message" && argc >= 5) return message(argv[2], argv[3], argv[4]);
  if (mode == L"folder" && argc >= 3) return folder(argv[2], argc >= 4 ? argv[3] : L"");
  return 2;
}
