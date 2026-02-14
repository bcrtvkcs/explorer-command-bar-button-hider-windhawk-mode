// ==WindhawkMod==
// @id explorer-command-bar-button-hider
// @name Explorer Command Bar Button Hider
// @description Hide specific buttons from the Windows 11 File Explorer command bar
// @version 2.0
// @author bcrtvkcs
// @github https://github.com/bcrtvkcs
// @include explorer.exe
// @architecture x86-64
// @compilerOptions -lole32 -loleaut32 -lruntimeobject -Wl,--export-all-symbols
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Explorer Command Bar Button Hider

Hides specific buttons from the Windows 11 File Explorer command bar.

You can choose which buttons to hide from the mod settings.

## How it works

The mod hooks `CreateWindowExW` to detect when a File Explorer window
(`CabinetWClass`) is created, then injects a XAML diagnostics TAP to monitor
the visual tree. When `AppBarButton` elements become visible, it checks their
icon's SVG URI to identify rotate/wallpaper buttons, and hides them.

*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- hideRotateLeft: true
  $name: Hide Rotate Left button
  $description: Hides the rotate left button from the command bar
- hideRotateRight: true
  $name: Hide Rotate Right button
  $description: Hides the rotate right button from the command bar
- hideSetAsDesktopBackground: true
  $name: Hide Set as Desktop Background button
  $description: Hides the set as desktop background button from the command bar
- customLabels:
  - - label: ""
      $name: Button label text
  $name: Custom labels to hide
  $description: Add additional button labels to hide by their visible text (exact match, case-insensitive)
*/
// ==/WindhawkModSettings==

// Source code is published under The GNU General Public License v3.0.

#include <xamlom.h>

#include <atomic>
#include <string>
#include <vector>

#undef GetCurrentTime

#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Windows.Foundation.h>

// ============================================================================
// Globals
// ============================================================================

std::atomic<bool> g_initialized;
bool g_disabled = false;

struct {
    bool hideRotateLeft;
    bool hideRotateRight;
    bool hideSetAsDesktopBackground;
    std::vector<std::wstring> customLabels;
} g_settings;

namespace mux = winrt::Microsoft::UI::Xaml;
namespace muxc = winrt::Microsoft::UI::Xaml::Controls;
namespace muxm = winrt::Microsoft::UI::Xaml::Media;
namespace wf = winrt::Windows::Foundation;

// Known SVG icon filenames (language-independent)
static const wchar_t* ICON_ROTATE_LEFT = L"windows.rotate270.svg";
static const wchar_t* ICON_ROTATE_RIGHT = L"windows.rotate90.svg";
static const wchar_t* ICON_WALLPAPER = L"windows.setdesktopwallpaper.svg";

// ============================================================================
// Icon and label matching
// ============================================================================

static std::wstring GetButtonSvgUri(mux::FrameworkElement element) {
    try {
        auto abb = element.try_as<muxc::AppBarButton>();
        if (!abb) return L"";
        auto icon = abb.Icon();
        if (!icon) return L"";
        auto imageIcon = icon.try_as<muxc::ImageIcon>();
        if (!imageIcon) return L"";
        auto source = imageIcon.Source();
        if (!source) return L"";
        auto svg = source.try_as<muxm::Imaging::SvgImageSource>();
        if (!svg) return L"";
        auto uri = svg.UriSource();
        if (!uri) return L"";
        return std::wstring(uri.AbsoluteUri());
    } catch (...) {}
    return L"";
}

static bool ShouldHideByIcon(const std::wstring& svgUri) {
    if (svgUri.empty()) return false;
    if (g_settings.hideRotateLeft &&
        svgUri.find(ICON_ROTATE_LEFT) != std::wstring::npos) return true;
    if (g_settings.hideRotateRight &&
        svgUri.find(ICON_ROTATE_RIGHT) != std::wstring::npos) return true;
    if (g_settings.hideSetAsDesktopBackground &&
        svgUri.find(ICON_WALLPAPER) != std::wstring::npos) return true;
    return false;
}

static bool ShouldHideByLabel(const std::wstring& labelText) {
    if (labelText.empty()) return false;
    for (const auto& custom : g_settings.customLabels) {
        if (_wcsicmp(labelText.c_str(), custom.c_str()) == 0) return true;
    }
    return false;
}

// ============================================================================
// Visual tree helpers
// ============================================================================

static mux::FrameworkElement FindChildByName(mux::DependencyObject parent, const winrt::hstring& name) {
    if (!parent) return nullptr;
    int count = muxm::VisualTreeHelper::GetChildrenCount(parent);
    for (int i = 0; i < count; i++) {
        auto child = muxm::VisualTreeHelper::GetChild(parent, i);
        auto fe = child.try_as<mux::FrameworkElement>();
        if (fe && fe.Name() == name) return fe;
        auto result = FindChildByName(child, name);
        if (result) return result;
    }
    return nullptr;
}

static std::wstring GetAppBarButtonLabelText(mux::FrameworkElement element) {
    auto textLabel = FindChildByName(element, L"TextLabel");
    if (textLabel) {
        auto tb = textLabel.try_as<muxc::TextBlock>();
        if (tb) return std::wstring(tb.Text());
    }
    auto abb = element.try_as<muxc::AppBarButton>();
    if (abb) {
        winrt::hstring label = abb.Label();
        if (!label.empty()) return std::wstring(label);
    }
    return L"";
}

// ============================================================================
// Button processing
// ============================================================================

static void HideAdjacentSeparator(mux::FrameworkElement hiddenButton) {
    auto parent = muxm::VisualTreeHelper::GetParent(hiddenButton);
    if (!parent) return;
    int childCount = muxm::VisualTreeHelper::GetChildrenCount(parent);
    int btnIndex = -1;
    for (int i = 0; i < childCount; i++) {
        auto child = muxm::VisualTreeHelper::GetChild(parent, i).try_as<mux::UIElement>();
        if (child && child == hiddenButton.try_as<mux::UIElement>()) { btnIndex = i; break; }
    }
    if (btnIndex < 0) return;
    for (int i = btnIndex - 1; i >= 0; i--) {
        auto child = muxm::VisualTreeHelper::GetChild(parent, i).try_as<mux::UIElement>();
        if (!child || child.Visibility() != mux::Visibility::Visible) continue;
        std::wstring cn(winrt::get_class_name(child));
        if (cn.find(L"AppBarSeparator") != std::wstring::npos) {
            child.Visibility(mux::Visibility::Collapsed);
        }
        break;
    }
}

static void ReHideCallback(mux::DependencyObject const& sender, mux::DependencyProperty const&) {
    if (g_disabled) return;
    auto el = sender.try_as<mux::FrameworkElement>();
    if (!el || el.Visibility() == mux::Visibility::Collapsed) return;
    std::wstring uri = GetButtonSvgUri(el);
    std::wstring label = GetAppBarButtonLabelText(el);
    if (ShouldHideByIcon(uri) || ShouldHideByLabel(label)) {
        Wh_Log(L"Re-hiding: %s", label.c_str());
        el.Visibility(mux::Visibility::Collapsed);
        HideAdjacentSeparator(el);
    }
}

static void HideButton(mux::FrameworkElement fe, const std::wstring& reason) {
    Wh_Log(L"Hiding button: %s", reason.c_str());
    fe.Visibility(mux::Visibility::Collapsed);
    HideAdjacentSeparator(fe);
    fe.RegisterPropertyChangedCallback(mux::UIElement::VisibilityProperty(), ReHideCallback);
}

static void ProcessAppBarButton(mux::FrameworkElement element) {
    if (!element) return;

    std::wstring labelText = GetAppBarButtonLabelText(element);
    std::wstring svgUri = GetButtonSvgUri(element);

    if (ShouldHideByIcon(svgUri)) {
        HideButton(element, labelText.empty() ? svgUri : labelText);
        return;
    }
    if (!labelText.empty() && ShouldHideByLabel(labelText)) {
        HideButton(element, labelText);
        return;
    }

    // If both empty, register visibility callback for deferred check
    if (labelText.empty() && svgUri.empty()) {
        element.RegisterPropertyChangedCallback(
            mux::UIElement::VisibilityProperty(),
            [](mux::DependencyObject const& sender, mux::DependencyProperty const&) {
                if (g_disabled) return;
                auto fe = sender.try_as<mux::FrameworkElement>();
                if (!fe || fe.Visibility() != mux::Visibility::Visible) return;

                std::wstring uri = GetButtonSvgUri(fe);
                std::wstring label = GetAppBarButtonLabelText(fe);

                if (ShouldHideByIcon(uri) || ShouldHideByLabel(label)) {
                    Wh_Log(L"Deferred hiding: %s",
                        label.empty() ? uri.c_str() : label.c_str());
                    fe.Visibility(mux::Visibility::Collapsed);
                    HideAdjacentSeparator(fe);
                    fe.RegisterPropertyChangedCallback(
                        mux::UIElement::VisibilityProperty(), ReHideCallback);
                    return;
                }

                if (uri.empty() && label.empty()) {
                    auto refFe = fe;
                    fe.DispatcherQueue().TryEnqueue(
                        winrt::Microsoft::UI::Dispatching::DispatcherQueuePriority::Low,
                        [refFe]() {
                        if (g_disabled) return;
                        std::wstring u = GetButtonSvgUri(refFe);
                        std::wstring l = GetAppBarButtonLabelText(refFe);
                        if (ShouldHideByIcon(u) || ShouldHideByLabel(l)) {
                            Wh_Log(L"Layout-deferred hide: %s",
                                l.empty() ? u.c_str() : l.c_str());
                            refFe.Visibility(mux::Visibility::Collapsed);
                            HideAdjacentSeparator(refFe);
                        }
                    });
                }
            });
    }
}

// ============================================================================
// VisualTreeWatcher
// ============================================================================

HMODULE GetCurrentModuleHandle() {
    HMODULE module;
    if (!GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           L"", &module)) {
        return nullptr;
    }
    return module;
}

class VisualTreeWatcher : public winrt::implements<VisualTreeWatcher, IVisualTreeServiceCallback2, winrt::non_agile>
{
public:
    VisualTreeWatcher(winrt::com_ptr<IUnknown> site);
    ~VisualTreeWatcher();
    void UnadviseVisualTreeChange();

    VisualTreeWatcher(const VisualTreeWatcher&) = delete;
    VisualTreeWatcher& operator=(const VisualTreeWatcher&) = delete;

private:
    HRESULT STDMETHODCALLTYPE OnVisualTreeChange(ParentChildRelation relation,
        VisualElement element, VisualMutationType mutationType) override;
    HRESULT STDMETHODCALLTYPE OnElementStateChanged(InstanceHandle,
        VisualElementState, LPCWSTR) noexcept override { return S_OK; }

    wf::IInspectable FromHandle(InstanceHandle handle) {
        wf::IInspectable obj;
        winrt::check_hresult(m_XamlDiagnostics->GetIInspectableFromHandle(
            handle, reinterpret_cast<::IInspectable**>(winrt::put_abi(obj))));
        return obj;
    }

    winrt::com_ptr<IXamlDiagnostics> m_XamlDiagnostics = nullptr;
};

VisualTreeWatcher::VisualTreeWatcher(winrt::com_ptr<IUnknown> site) :
    m_XamlDiagnostics(site.as<IXamlDiagnostics>())
{
    Wh_Log(L"Constructing VisualTreeWatcher");

    HANDLE thread = CreateThread(
        nullptr, 0,
        [](LPVOID lpParam) -> DWORD {
            auto watcher = reinterpret_cast<VisualTreeWatcher*>(lpParam);
            HRESULT hr = watcher->m_XamlDiagnostics.as<IVisualTreeService3>()->AdviseVisualTreeChange(watcher);
            watcher->Release();
            if (FAILED(hr)) {
                Wh_Log(L"AdviseVisualTreeChange error %08X", hr);
            }
            return 0;
        },
        this, 0, nullptr);
    if (thread) {
        AddRef();
        CloseHandle(thread);
    }
}

VisualTreeWatcher::~VisualTreeWatcher() {
    Wh_Log(L"Destructing VisualTreeWatcher");
}

void VisualTreeWatcher::UnadviseVisualTreeChange() {
    Wh_Log(L"UnadviseVisualTreeChange");
    HRESULT hr = m_XamlDiagnostics.as<IVisualTreeService3>()->UnadviseVisualTreeChange(this);
    if (FAILED(hr)) {
        Wh_Log(L"UnadviseVisualTreeChange failed: %08X", hr);
    }
}

HRESULT VisualTreeWatcher::OnVisualTreeChange(
    ParentChildRelation, VisualElement element, VisualMutationType mutationType) try
{
    if (g_disabled || mutationType != Add || !element.Type) return S_OK;

    std::wstring_view typeName(element.Type);

    // Strategy 1: AppBarButton directly added
    if (typeName.find(L"AppBarButton") != std::wstring_view::npos) {
        try {
            auto obj = FromHandle(element.Handle);
            auto fe = obj.try_as<mux::FrameworkElement>();
            if (fe) ProcessAppBarButton(fe);
        } catch (...) {}
        return S_OK;
    }

    // Strategy 2: TextLabel added — walk up to find AppBarButton
    std::wstring_view elName(element.Name ? element.Name : L"");
    if (elName == L"TextLabel" && typeName.find(L"TextBlock") != std::wstring_view::npos) {
        try {
            auto obj = FromHandle(element.Handle);
            auto fe = obj.try_as<mux::FrameworkElement>();
            if (fe) {
                auto current = muxm::VisualTreeHelper::GetParent(fe);
                for (int depth = 0; depth < 10 && current; depth++) {
                    auto parentFe = current.try_as<mux::FrameworkElement>();
                    if (parentFe) {
                        std::wstring cn(winrt::get_class_name(parentFe));
                        if (cn.find(L"AppBarButton") != std::wstring::npos) {
                            ProcessAppBarButton(parentFe);
                            break;
                        }
                    }
                    current = muxm::VisualTreeHelper::GetParent(current);
                }
            }
        } catch (...) {}
    }

    return S_OK;
}
catch (...)
{
    Wh_Log(L"OnVisualTreeChange error %08X", (unsigned)winrt::to_hresult());
    return S_OK;
}

// ============================================================================
// TAP (Technical Access Provider) — COM class for XAML diagnostics
// ============================================================================

winrt::com_ptr<VisualTreeWatcher> g_visualTreeWatcher;

// {D7B8DB42-7A9F-4E14-8C1A-6E3B72F8A5C1}
static constexpr CLSID CLSID_WindhawkTAP = {
    0xd7b8db42, 0x7a9f, 0x4e14,
    { 0x8c, 0x1a, 0x6e, 0x3b, 0x72, 0xf8, 0xa5, 0xc1 }
};

class WindhawkTAP : public winrt::implements<WindhawkTAP, IObjectWithSite, winrt::non_agile>
{
public:
    HRESULT STDMETHODCALLTYPE SetSite(IUnknown *pUnkSite) override;
    HRESULT STDMETHODCALLTYPE GetSite(REFIID riid, void **ppvSite) noexcept override;
private:
    winrt::com_ptr<IUnknown> site;
};

HRESULT WindhawkTAP::SetSite(IUnknown *pUnkSite) try {
    site.copy_from(pUnkSite);
    if (pUnkSite) {
        if (g_visualTreeWatcher) {
            g_visualTreeWatcher->UnadviseVisualTreeChange();
            g_visualTreeWatcher = nullptr;
        }
        g_visualTreeWatcher = winrt::make_self<VisualTreeWatcher>(site);
    } else {
        if (g_visualTreeWatcher) {
            g_visualTreeWatcher->UnadviseVisualTreeChange();
            g_visualTreeWatcher = nullptr;
        }
    }
    return S_OK;
}
catch (...) { return winrt::to_hresult(); }

HRESULT WindhawkTAP::GetSite(REFIID riid, void **ppvSite) noexcept {
    return site.as(riid, ppvSite);
}

// ============================================================================
// Factory & DLL exports
// ============================================================================

template<class T>
struct SimpleFactory : winrt::implements<SimpleFactory<T>, IClassFactory, winrt::non_agile>
{
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppvObject) override try {
        if (!pUnkOuter) {
            *ppvObject = nullptr;
            return winrt::make<T>().as(riid, ppvObject);
        }
        return CLASS_E_NOAGGREGATION;
    }
    catch (...) { return winrt::to_hresult(); }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL) noexcept override { return S_OK; }
};

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdll-attribute-on-redeclaration"

__declspec(dllexport)
_Use_decl_annotations_ STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv) try {
    if (rclsid == CLSID_WindhawkTAP) {
        *ppv = nullptr;
        return winrt::make<SimpleFactory<WindhawkTAP>>().as(riid, ppv);
    }
    return CLASS_E_CLASSNOTAVAILABLE;
}
catch (...) { return winrt::to_hresult(); }

__declspec(dllexport)
_Use_decl_annotations_ STDAPI DllCanUnloadNow(void) {
    return winrt::get_module_lock() ? S_FALSE : S_OK;
}

#pragma clang diagnostic pop

// ============================================================================
// TAP injection (uses FrameworkUdk + WinUI connection names)
// ============================================================================

using PFN_INITIALIZE_XAML_DIAGNOSTICS_EX = decltype(&InitializeXamlDiagnosticsEx);

bool g_inInjectWindhawkTAP = false;

HRESULT InjectWindhawkTAP() noexcept {
    HMODULE module = GetCurrentModuleHandle();
    if (!module) return HRESULT_FROM_WIN32(GetLastError());

    WCHAR location[MAX_PATH];
    switch (GetModuleFileName(module, location, ARRAYSIZE(location))) {
    case 0:
    case ARRAYSIZE(location):
        return HRESULT_FROM_WIN32(GetLastError());
    }

    // File Explorer uses WinUI 3, diagnostics in FrameworkUdk.dll
    const HMODULE wux(GetModuleHandle(L"Microsoft.Internal.FrameworkUdk.dll"));
    if (!wux) [[unlikely]] return HRESULT_FROM_WIN32(GetLastError());

    const auto ixde = reinterpret_cast<PFN_INITIALIZE_XAML_DIAGNOSTICS_EX>(
        GetProcAddress(wux, "InitializeXamlDiagnosticsEx"));
    if (!ixde) [[unlikely]] return HRESULT_FROM_WIN32(GetLastError());

    // Try multiple connection names until one works
    g_inInjectWindhawkTAP = true;

    HRESULT hr;
    for (int i = 0; i < 10000; i++) {
        WCHAR connectionName[256];
        wsprintf(connectionName, L"WinUIVisualDiagConnection%d", i + 1);

        hr = ixde(connectionName, GetCurrentProcessId(), L"", location, CLSID_WindhawkTAP, nullptr);
        if (hr != HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) {
            break;
        }
    }

    g_inInjectWindhawkTAP = false;

    return hr;
}

void InitializeSettingsAndTap() {
    if (g_initialized.exchange(true)) return;

    Wh_Log(L"Injecting TAP");
    HRESULT hr = InjectWindhawkTAP();
    if (FAILED(hr)) {
        Wh_Log(L"InjectWindhawkTAP error %08X", hr);
    }
}

void UninitializeSettingsAndTap() {
    g_initialized = false;
    if (g_visualTreeWatcher) {
        g_visualTreeWatcher->UnadviseVisualTreeChange();
        g_visualTreeWatcher = nullptr;
    }
}

// ============================================================================
// CreateWindowExW hook
// ============================================================================

bool IsTargetWindow(HWND hWnd) {
    WCHAR className[64];
    if (!GetClassName(hWnd, className, ARRAYSIZE(className))) return false;
    return _wcsicmp(className, L"CabinetWClass") == 0;
}

using CreateWindowExW_t = decltype(&CreateWindowExW);
CreateWindowExW_t CreateWindowExW_Original;

HWND WINAPI CreateWindowExW_Hook(DWORD dwExStyle, LPCWSTR lpClassName,
    LPCWSTR lpWindowName, DWORD dwStyle, int X, int Y,
    int nWidth, int nHeight, HWND hWndParent, HMENU hMenu,
    HINSTANCE hInstance, PVOID lpParam)
{
    HWND hWnd = CreateWindowExW_Original(dwExStyle, lpClassName, lpWindowName,
        dwStyle, X, Y, nWidth, nHeight, hWndParent, hMenu, hInstance, lpParam);
    if (!hWnd) return hWnd;

    if (IsTargetWindow(hWnd)) {
        Wh_Log(L"Explorer window: hwnd=%08X", (DWORD)(ULONG_PTR)hWnd);
        InitializeSettingsAndTap();
    }
    return hWnd;
}

// ============================================================================
// Settings
// ============================================================================

void LoadSettings() {
    g_settings.hideRotateLeft = Wh_GetIntSetting(L"hideRotateLeft");
    g_settings.hideRotateRight = Wh_GetIntSetting(L"hideRotateRight");
    g_settings.hideSetAsDesktopBackground = Wh_GetIntSetting(L"hideSetAsDesktopBackground");

    g_settings.customLabels.clear();
    for (int i = 0; ; i++) {
        WCHAR name[256];
        swprintf_s(name, L"customLabels[%d].label", i);
        PCWSTR val = Wh_GetStringSetting(name);
        if (!val || !*val) { Wh_FreeStringSetting(val); break; }
        g_settings.customLabels.push_back(val);
        Wh_FreeStringSetting(val);
    }

    Wh_Log(L"Settings: RotateLeft=%d RotateRight=%d Wallpaper=%d Custom=%zu",
            g_settings.hideRotateLeft, g_settings.hideRotateRight,
            g_settings.hideSetAsDesktopBackground, g_settings.customLabels.size());
}

// ============================================================================
// Find existing windows
// ============================================================================

std::vector<HWND> GetExistingExplorerWindows() {
    struct EnumParam { std::vector<HWND>* hWnds; DWORD pid; };
    std::vector<HWND> hWnds;
    EnumParam param = { &hWnds, GetCurrentProcessId() };
    EnumWindows([](HWND hWnd, LPARAM lParam) -> BOOL {
        auto* p = reinterpret_cast<EnumParam*>(lParam);
        DWORD pid;
        GetWindowThreadProcessId(hWnd, &pid);
        if (pid == p->pid && IsTargetWindow(hWnd)) p->hWnds->push_back(hWnd);
        return TRUE;
    }, (LPARAM)&param);
    return hWnds;
}

// ============================================================================
// Mod lifecycle
// ============================================================================

BOOL Wh_ModInit() {
    Wh_Log(L">");
    LoadSettings();
    g_disabled = false;

    Wh_SetFunctionHook((void*)CreateWindowExW,
                        (void*)CreateWindowExW_Hook,
                        (void**)&CreateWindowExW_Original);
    return TRUE;
}

void Wh_ModAfterInit() {
    Wh_Log(L">");
    auto hWnds = GetExistingExplorerWindows();
    if (!hWnds.empty()) {
        Wh_Log(L"Found %zu existing Explorer windows", hWnds.size());
        InitializeSettingsAndTap();
    }
}

void Wh_ModUninit() {
    Wh_Log(L">");
    g_disabled = true;
    UninitializeSettingsAndTap();
}

BOOL Wh_ModSettingsChanged(BOOL* bReload) {
    Wh_Log(L"Settings changed");
    LoadSettings();
    *bReload = TRUE;
    return TRUE;
}