// ===========================================================================
//  dlssnr_page.h -- DirectShow property page (IPropertyPage)
//
//  This is what makes "Filters -> DLSSNR -> Properties" work inside the player,
//  the same way LAV exposes its settings. The page hosts the very same settings
//  window the tray icon opens (dlssnr_ui.h), so there is exactly one UI to
//  maintain.
//
//  Registered as its own in-proc CLSID; ISpecifyPropertyPages::GetPages on the
//  filter hands this CLSID to the player's property frame, which then
//  CoCreateInstance()s it.
// ===========================================================================
#pragma once

#include <ocidl.h>

class CDlssNrPage : public IPropertyPage {
public:
    CDlssNrPage() : m_ref(1), m_site(nullptr), m_frame(nullptr), m_hwnd(nullptr),
                    m_objs(0), m_dirty(false) {}

    // ---- IUnknown ----
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IPropertyPage) {
            *ppv = static_cast<IPropertyPage*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() {
        LONG r = InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return r;
    }

    // ---- IPropertyPage ----
    STDMETHODIMP SetPageSite(IPropertyPageSite* site) {
        if (m_site) { m_site->Release(); m_site = nullptr; }
        if (site) { site->AddRef(); m_site = site; }
        return S_OK;
    }

    STDMETHODIMP Activate(HWND frame, LPCRECT prc, BOOL /*bModal*/) {
        m_frame = frame;
        m_hwnd = DlssNrCreateSettings(frame, false);
        if (!m_hwnd) return E_FAIL;
        if (m_ui()) {
            m_ui()->onChanged    = &CDlssNrPage::OnUiChangedThunk;
            m_ui()->onChangedCtx = this;
        }
        if (prc) Move(prc);
        Show(SW_SHOW);
        return S_OK;
    }

    STDMETHODIMP Deactivate() {
        if (m_hwnd) { DestroyWindow(m_hwnd); m_hwnd = nullptr; }
        m_frame = nullptr;
        return S_OK;
    }

    STDMETHODIMP GetPageInfo(PROPPAGEINFO* info) {
        if (!info) return E_POINTER;
        info->cb = sizeof(*info);
        info->pszTitle = nullptr;
        info->pszDocString = nullptr;
        info->pszHelpFile = nullptr;
        info->dwHelpContext = 0;
        info->size.cx = 348;
        info->size.cy = 250;
        const wchar_t* t = L"DLSSNR";
        size_t cb = (wcslen(t) + 1) * sizeof(wchar_t);
        info->pszTitle = (LPOLESTR)CoTaskMemAlloc(cb);
        if (!info->pszTitle) return E_OUTOFMEMORY;
        memcpy(info->pszTitle, t, cb);
        return S_OK;
    }

    STDMETHODIMP SetObjects(ULONG n, IUnknown** /*objs*/) {
        m_objs = n;
        return S_OK;
    }

    STDMETHODIMP Show(UINT cmd) {
        if (m_hwnd) ShowWindow(m_hwnd, cmd);
        return S_OK;
    }

    STDMETHODIMP Move(LPCRECT prc) {
        if (!m_hwnd || !prc) return S_OK;
        SetWindowPos(m_hwnd, nullptr, prc->left, prc->top,
                     prc->right - prc->left, prc->bottom - prc->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return S_OK;
    }

    STDMETHODIMP IsPageDirty() { return m_dirty ? S_OK : S_FALSE; }

    // The controls have already been applied live (that is the whole point of
    // tuning while watching), so Apply only has to flush them to the ini.
    STDMETHODIMP Apply() {
        DlssNrPush(m_hwnd, true);
        m_dirty = false;
        if (m_site) m_site->OnStatusChange(PROPPAGESTATUS_CLEAN);
        return S_OK;
    }

    STDMETHODIMP Help(LPCOLESTR /*helpDir*/) { return E_NOTIMPL; }

    STDMETHODIMP TranslateAccelerator(MSG* /*pMsg*/) { return E_NOTIMPL; }

private:
    static void OnUiChangedThunk(void* ctx) { ((CDlssNrPage*)ctx)->OnUiChanged(); }
    void OnUiChanged() {
        m_dirty = true;
        if (m_site) m_site->OnStatusChange(PROPPAGESTATUS_DIRTY);
    }
    DlssNrSettings* m_ui() {
        return m_hwnd ? (DlssNrSettings*)GetWindowLongPtrW(m_hwnd, GWLP_USERDATA) : nullptr;
    }

    LONG               m_ref;
    IPropertyPageSite* m_site;
    HWND               m_frame;
    HWND               m_hwnd;
    ULONG              m_objs;
    bool               m_dirty;
};