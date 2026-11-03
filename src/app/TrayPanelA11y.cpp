#include "TrayPanelA11y.h"

#include <UIAutomation.h>
#include <oleauto.h>
#include <wrl/client.h>
#include <wrl/implements.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

using Microsoft::WRL::ComPtr;
using Kind = A11yElement::Kind;

// Not every SDK header carries these.
#ifndef UIA_E_ELEMENTNOTENABLED
#define UIA_E_ELEMENTNOTENABLED ((HRESULT)0x80040200L)
#endif
#ifndef UIA_E_ELEMENTNOTAVAILABLE
#define UIA_E_ELEMENTNOTAVAILABLE ((HRESULT)0x80040201L)
#endif
#ifndef UIA_E_INVALIDOPERATION
#define UIA_E_INVALIDOPERATION ((HRESULT)0x80131509L)
#endif

namespace {

constexpr int kRoot = -1;  // the panel's own id, outside every element's

void SetBstr(VARIANT* v, const std::wstring& s) {
    v->vt = VT_BSTR;
    v->bstrVal = SysAllocStringLen(s.c_str(), static_cast<UINT>(s.size()));
}

void SetBool(VARIANT* v, bool b) {
    v->vt = VT_BOOL;
    v->boolVal = b ? VARIANT_TRUE : VARIANT_FALSE;
}

CONTROLTYPEID ControlTypeOf(Kind k) {
    switch (k) {
    case Kind::Button: return UIA_ButtonControlTypeId;
    case Kind::Toggle: return UIA_CheckBoxControlTypeId;
    case Kind::Radio:  return UIA_RadioButtonControlTypeId;
    case Kind::Group:  return UIA_GroupControlTypeId;
    default:           return UIA_TextControlTypeId;
    }
}

}  // namespace

class Node;

struct PanelAccessibility::Impl {
    explicit Impl(Host& h) : host(h) {}

    Host&                host;
    A11yTree             tree;
    std::map<int, size_t> position;   // element id -> index in tree.elements
    std::map<int, ComPtr<Node>> nodes;
    ComPtr<Node>         root;
    bool                 detached = false;

    const A11yElement* Find(int id) const {
        auto it = position.find(id);
        return it == position.end() ? nullptr : &tree.elements[it->second];
    }
    ComPtr<Node> NodeFor(int id);
    std::vector<int> ChildrenOf(int parentId) const {
        std::vector<int> ids;
        for (const auto& e : tree.elements)
            if (e.parent == parentId) ids.push_back(e.id);
        return ids;
    }
};

// One provider class for every element, and for the panel itself. What it is is
// decided by looking its id up in the current description each time it is asked,
// so it never holds a stale answer, and a node whose element has gone away just
// reports that it is not available any more.
class Node final : public Microsoft::WRL::RuntimeClass<
                       Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                       IRawElementProviderSimple, IRawElementProviderFragment,
                       IRawElementProviderFragmentRoot, IInvokeProvider, IToggleProvider,
                       ISelectionItemProvider> {
public:
    Node(PanelAccessibility::Impl* owner, int id) : m_owner(owner), m_id(id) {}
    void Detach() { m_owner = nullptr; }
    int  Id() const { return m_id; }

    // IRawElementProviderSimple
    HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* options) override;
    HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID pattern, IUnknown** provider) override;
    HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID property, VARIANT* out) override;
    HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** host) override;

    // IRawElementProviderFragment
    HRESULT STDMETHODCALLTYPE Navigate(NavigateDirection direction, IRawElementProviderFragment** result) override;
    HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY** id) override;
    HRESULT STDMETHODCALLTYPE get_BoundingRectangle(UiaRect* rect) override;
    HRESULT STDMETHODCALLTYPE GetEmbeddedFragmentRoots(SAFEARRAY** roots) override;
    HRESULT STDMETHODCALLTYPE SetFocus() override;
    HRESULT STDMETHODCALLTYPE get_FragmentRoot(IRawElementProviderFragmentRoot** root) override;

    // IRawElementProviderFragmentRoot
    HRESULT STDMETHODCALLTYPE ElementProviderFromPoint(double x, double y, IRawElementProviderFragment** result) override;
    HRESULT STDMETHODCALLTYPE GetFocus(IRawElementProviderFragment** result) override;

    // Patterns
    HRESULT STDMETHODCALLTYPE Invoke() override;
    HRESULT STDMETHODCALLTYPE Toggle() override;
    HRESULT STDMETHODCALLTYPE get_ToggleState(ToggleState* state) override;
    HRESULT STDMETHODCALLTYPE Select() override;
    HRESULT STDMETHODCALLTYPE AddToSelection() override;
    HRESULT STDMETHODCALLTYPE RemoveFromSelection() override;
    HRESULT STDMETHODCALLTYPE get_IsSelected(BOOL* selected) override;
    HRESULT STDMETHODCALLTYPE get_SelectionContainer(IRawElementProviderSimple** container) override;

private:
    bool IsRoot() const { return m_id == kRoot; }
    bool Live() const { return m_owner && !m_owner->detached && (IsRoot() || m_owner->Find(m_id)); }
    const A11yElement* Elem() const { return (m_owner && !IsRoot()) ? m_owner->Find(m_id) : nullptr; }
    HRESULT Activate();
    HRESULT HandBack(int id, IRawElementProviderFragment** result);

    PanelAccessibility::Impl* m_owner;
    int                       m_id;
};

ComPtr<Node> PanelAccessibility::Impl::NodeFor(int id) {
    if (id == kRoot) return root;
    auto it = nodes.find(id);
    if (it != nodes.end()) return it->second;
    ComPtr<Node> n = Microsoft::WRL::Make<Node>(this, id);
    nodes[id] = n;
    return n;
}

// ---- Node: simple provider -------------------------------------------------

HRESULT Node::get_ProviderOptions(ProviderOptions* options) {
    // Calls arrive on the thread that made the provider, through COM, rather than
    // on one of UI Automation's own. The panel's state is only touched from its own
    // thread, and this is what lets it stay that way without a lock.
    *options = static_cast<ProviderOptions>(ProviderOptions_ServerSideProvider | ProviderOptions_UseComThreading);
    return S_OK;
}

HRESULT Node::GetPatternProvider(PATTERNID pattern, IUnknown** provider) {
    *provider = nullptr;
    if (!Live()) return UIA_E_ELEMENTNOTAVAILABLE;
    const A11yElement* e = Elem();
    if (!e) return S_OK;
    const bool wanted = (pattern == UIA_InvokePatternId        && e->kind == Kind::Button) ||
                        (pattern == UIA_TogglePatternId        && e->kind == Kind::Toggle) ||
                        (pattern == UIA_SelectionItemPatternId && e->kind == Kind::Radio);
    if (!wanted) return S_OK;
    return QueryInterface(pattern == UIA_InvokePatternId ? __uuidof(IInvokeProvider)
                        : pattern == UIA_TogglePatternId ? __uuidof(IToggleProvider)
                                                         : __uuidof(ISelectionItemProvider),
                          reinterpret_cast<void**>(provider));
}

HRESULT Node::GetPropertyValue(PROPERTYID property, VARIANT* out) {
    VariantInit(out);
    if (!Live()) return UIA_E_ELEMENTNOTAVAILABLE;
    const A11yElement* e = Elem();

    switch (property) {
    case UIA_ControlTypePropertyId:
        out->vt = VT_I4;
        out->lVal = e ? ControlTypeOf(e->kind) : UIA_PaneControlTypeId;
        break;
    case UIA_NamePropertyId:
        SetBstr(out, e ? e->name : m_owner->tree.name);
        break;
    case UIA_HelpTextPropertyId:
        if (e && !e->help.empty()) SetBstr(out, e->help);
        break;
    case UIA_AutomationIdPropertyId:
        if (e && !e->automationId.empty()) SetBstr(out, e->automationId);
        break;
    case UIA_FrameworkIdPropertyId:
        SetBstr(out, L"SteamlessTray");
        break;
    case UIA_IsControlElementPropertyId:
    case UIA_IsContentElementPropertyId:
        SetBool(out, true);
        break;
    case UIA_IsEnabledPropertyId:
        SetBool(out, e ? e->enabled : true);
        break;
    case UIA_IsKeyboardFocusablePropertyId:
        SetBool(out, e && e->kind != Kind::Text && e->kind != Kind::Group && e->enabled);
        break;
    case UIA_HasKeyboardFocusPropertyId:
        SetBool(out, e && m_owner->tree.focusId == e->id);
        break;
    case UIA_IsOffscreenPropertyId:
        SetBool(out, false);
        break;
    case UIA_PositionInSetPropertyId:
        if (e && e->position > 0) { out->vt = VT_I4; out->lVal = e->position; }
        break;
    case UIA_SizeOfSetPropertyId:
        if (e && e->size > 0) { out->vt = VT_I4; out->lVal = e->size; }
        break;
    default:
        break;  // VT_EMPTY: not something this provider supplies
    }
    return S_OK;
}

HRESULT Node::get_HostRawElementProvider(IRawElementProviderSimple** host) {
    *host = nullptr;
    if (!Live()) return UIA_E_ELEMENTNOTAVAILABLE;
    // The panel's window already has a provider of its own — its title, its place
    // among the desktop's windows. The root joins ours onto it; everything below
    // it has no host of its own.
    if (IsRoot()) return UiaHostProviderFromHwnd(m_owner->host.Window(), host);
    return S_OK;
}

// ---- Node: fragment provider -----------------------------------------------

HRESULT Node::HandBack(int id, IRawElementProviderFragment** result) {
    *result = nullptr;
    if (id == kRoot - 1) return S_OK;  // "nobody", see Navigate
    ComPtr<Node> n = m_owner->NodeFor(id);
    return n.CopyTo(result);
}

HRESULT Node::Navigate(NavigateDirection direction, IRawElementProviderFragment** result) {
    *result = nullptr;
    if (!Live()) return UIA_E_ELEMENTNOTAVAILABLE;

    const int none = kRoot - 1;
    const A11yElement* e = Elem();
    const int parent = e ? e->parent : none;

    switch (direction) {
    case NavigateDirection_Parent:
        return e ? HandBack(parent, result) : S_OK;

    case NavigateDirection_FirstChild:
    case NavigateDirection_LastChild: {
        const std::vector<int> kids = m_owner->ChildrenOf(m_id);
        if (kids.empty()) return S_OK;
        return HandBack(direction == NavigateDirection_FirstChild ? kids.front() : kids.back(), result);
    }

    case NavigateDirection_NextSibling:
    case NavigateDirection_PreviousSibling: {
        if (!e) return S_OK;  // the panel has no siblings
        const std::vector<int> sibs = m_owner->ChildrenOf(parent);
        const auto at = std::find(sibs.begin(), sibs.end(), m_id);
        if (at == sibs.end()) return S_OK;
        if (direction == NavigateDirection_NextSibling)
            return (at + 1 != sibs.end()) ? HandBack(*(at + 1), result) : S_OK;
        return (at != sibs.begin()) ? HandBack(*(at - 1), result) : S_OK;
    }
    }
    return S_OK;
}

HRESULT Node::GetRuntimeId(SAFEARRAY** id) {
    *id = nullptr;
    if (!Live()) return UIA_E_ELEMENTNOTAVAILABLE;
    if (IsRoot()) return S_OK;  // the window's own provider supplies this one
    SAFEARRAY* sa = SafeArrayCreateVector(VT_I4, 0, 2);
    if (!sa) return E_OUTOFMEMORY;
    LONG index = 0;
    int  out = UiaAppendRuntimeId;
    SafeArrayPutElement(sa, &index, &out);
    index = 1;
    out = m_id + 1;  // ids start at 0, and a runtime id part of 0 is not allowed to mean anything
    SafeArrayPutElement(sa, &index, &out);
    *id = sa;
    return S_OK;
}

HRESULT Node::get_BoundingRectangle(UiaRect* rect) {
    *rect = UiaRect{};
    if (!Live()) return UIA_E_ELEMENTNOTAVAILABLE;
    HWND hwnd = m_owner->host.Window();
    if (!hwnd) return UIA_E_ELEMENTNOTAVAILABLE;
    if (IsRoot()) {
        RECT rc{};
        GetWindowRect(hwnd, &rc);
        *rect = UiaRect{ static_cast<double>(rc.left), static_cast<double>(rc.top),
                         static_cast<double>(rc.right - rc.left), static_cast<double>(rc.bottom - rc.top) };
        return S_OK;
    }
    const A11yElement* e = Elem();
    const float s = m_owner->host.Scale();
    POINT origin{ 0, 0 };
    ClientToScreen(hwnd, &origin);
    rect->left   = origin.x + static_cast<double>(e->left * s);
    rect->top    = origin.y + static_cast<double>(e->top * s);
    rect->width  = static_cast<double>((e->right - e->left) * s);
    rect->height = static_cast<double>((e->bottom - e->top) * s);
    return S_OK;
}

HRESULT Node::GetEmbeddedFragmentRoots(SAFEARRAY** roots) {
    *roots = nullptr;
    return S_OK;
}

HRESULT Node::SetFocus() {
    if (!Live()) return UIA_E_ELEMENTNOTAVAILABLE;
    if (!IsRoot()) m_owner->host.SetFocus(m_id);
    return S_OK;
}

HRESULT Node::get_FragmentRoot(IRawElementProviderFragmentRoot** root) {
    *root = nullptr;
    if (!Live()) return UIA_E_ELEMENTNOTAVAILABLE;
    return m_owner->root.CopyTo(root);
}

// ---- Node: fragment root ---------------------------------------------------

HRESULT Node::ElementProviderFromPoint(double x, double y, IRawElementProviderFragment** result) {
    *result = nullptr;
    if (!Live()) return UIA_E_ELEMENTNOTAVAILABLE;
    HWND hwnd = m_owner->host.Window();
    if (!hwnd) return UIA_E_ELEMENTNOTAVAILABLE;
    POINT origin{ 0, 0 };
    ClientToScreen(hwnd, &origin);
    const float s = m_owner->host.Scale();
    const float cx = static_cast<float>((x - origin.x) / static_cast<double>(s));
    const float cy = static_cast<float>((y - origin.y) / static_cast<double>(s));

    // The last match, since a child comes after the group it sits in and is the
    // more specific answer.
    int found = kRoot;
    for (const auto& e : m_owner->tree.elements)
        if (cx >= e.left && cx < e.right && cy >= e.top && cy < e.bottom) found = e.id;
    return HandBack(found, result);
}

HRESULT Node::GetFocus(IRawElementProviderFragment** result) {
    *result = nullptr;
    if (!Live()) return UIA_E_ELEMENTNOTAVAILABLE;
    const int id = m_owner->tree.focusId;
    return (id >= 0 && m_owner->Find(id)) ? HandBack(id, result) : S_OK;
}

// ---- Node: patterns --------------------------------------------------------

HRESULT Node::Activate() {
    if (!Live()) return UIA_E_ELEMENTNOTAVAILABLE;
    const A11yElement* e = Elem();
    if (!e) return UIA_E_INVALIDOPERATION;
    if (!e->enabled) return UIA_E_ELEMENTNOTENABLED;
    // May close the panel, and with it this window and every node. Nothing here
    // touches them afterwards.
    m_owner->host.Invoke(m_id);
    return S_OK;
}

HRESULT Node::Invoke() { return Activate(); }
HRESULT Node::Toggle() { return Activate(); }
HRESULT Node::Select() { return Activate(); }

HRESULT Node::get_ToggleState(ToggleState* state) {
    *state = ToggleState_Off;
    if (!Live()) return UIA_E_ELEMENTNOTAVAILABLE;
    const A11yElement* e = Elem();
    if (e && e->on) *state = ToggleState_On;
    return S_OK;
}

// Exactly one of the modes is always chosen; selecting more, or none, is not a
// thing the control can do.
HRESULT Node::AddToSelection() { return UIA_E_INVALIDOPERATION; }
HRESULT Node::RemoveFromSelection() { return UIA_E_INVALIDOPERATION; }

HRESULT Node::get_IsSelected(BOOL* selected) {
    *selected = FALSE;
    if (!Live()) return UIA_E_ELEMENTNOTAVAILABLE;
    const A11yElement* e = Elem();
    *selected = (e && e->on) ? TRUE : FALSE;
    return S_OK;
}

HRESULT Node::get_SelectionContainer(IRawElementProviderSimple** container) {
    *container = nullptr;
    if (!Live()) return UIA_E_ELEMENTNOTAVAILABLE;
    const A11yElement* e = Elem();
    if (!e || e->parent == kRoot) return S_OK;
    ComPtr<Node> group = m_owner->NodeFor(e->parent);
    return group.CopyTo(container);
}

// ---- PanelAccessibility ----------------------------------------------------

PanelAccessibility::PanelAccessibility(Host& host) : m_d(std::make_unique<Impl>(host)) {
    // The panel's thread has to be a single-threaded apartment for providers that
    // ask for COM threading. The app's UI thread already is one — WebView2 needs it
    // — so this is normally a no-op; it is here so the panel does not depend on
    // that. Never undone, for the same reason TouchKeyboard never does: nothing on
    // this thread outlives the process that would need it.
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    m_d->root = Microsoft::WRL::Make<Node>(m_d.get(), kRoot);
}

PanelAccessibility::~PanelAccessibility() { Detach(); }

bool PanelAccessibility::ClientsListening() { return UiaClientsAreListening() != FALSE; }

bool PanelAccessibility::WantsRoot(LPARAM lp) {
    return static_cast<LONG>(lp) == static_cast<LONG>(UiaRootObjectId);
}

LRESULT PanelAccessibility::HandleGetObject(WPARAM wp, LPARAM lp) {
    if (m_d->detached || !m_d->root) return 0;
    return UiaReturnRawElementProvider(m_d->host.Window(), wp, lp,
                                       static_cast<IRawElementProviderSimple*>(m_d->root.Get()));
}

void PanelAccessibility::Detach() {
    if (m_d->detached) return;
    m_d->detached = true;
    if (m_d->root) {
        UiaDisconnectProvider(static_cast<IRawElementProviderSimple*>(m_d->root.Get()));
        m_d->root->Detach();
    }
    for (auto& [id, node] : m_d->nodes) {
        UiaDisconnectProvider(static_cast<IRawElementProviderSimple*>(node.Get()));
        node->Detach();
    }
    m_d->nodes.clear();
}

void PanelAccessibility::Update(A11yTree next) {
    Impl& d = *m_d;
    if (d.detached) return;

    A11yTree prev = std::move(d.tree);
    d.tree = std::move(next);
    d.position.clear();
    for (size_t i = 0; i < d.tree.elements.size(); ++i) d.position[d.tree.elements[i].id] = i;

    // Nothing below is worth doing unless somebody is listening.
    if (!UiaClientsAreListening()) return;

    auto simple = [&](int id) -> ComPtr<IRawElementProviderSimple> {
        ComPtr<IRawElementProviderSimple> p;
        d.NodeFor(id).As(&p);
        return p;
    };

    // Elements that came or went, or moved to a different parent: the old picture
    // of the tree is no good, so say so and let the listener ask again.
    bool structural = prev.elements.size() != d.tree.elements.size();
    for (size_t i = 0; !structural && i < prev.elements.size(); ++i)
        structural = prev.elements[i].id != d.tree.elements[i].id ||
                     prev.elements[i].parent != d.tree.elements[i].parent;
    if (structural) {
        if (auto root = simple(kRoot))
            UiaRaiseStructureChangedEvent(root.Get(), StructureChangeType_ChildrenInvalidated, nullptr, 0);
    }

    // Values that changed on elements that were there before and still are. Only
    // what a screen reader announces: names, state, and whether it can be used.
    std::map<int, const A11yElement*> before;
    for (const auto& e : prev.elements) before[e.id] = &e;
    for (const auto& e : d.tree.elements) {
        auto it = before.find(e.id);
        if (it == before.end()) continue;
        const A11yElement& old = *it->second;
        ComPtr<IRawElementProviderSimple> p = simple(e.id);
        if (!p) continue;

        VARIANT from{}, to{};
        if (old.name != e.name) {
            SetBstr(&from, old.name); SetBstr(&to, e.name);
            UiaRaiseAutomationPropertyChangedEvent(p.Get(), UIA_NamePropertyId, from, to);
            VariantClear(&from); VariantClear(&to);
        }
        if (old.help != e.help) {
            SetBstr(&from, old.help); SetBstr(&to, e.help);
            UiaRaiseAutomationPropertyChangedEvent(p.Get(), UIA_HelpTextPropertyId, from, to);
            VariantClear(&from); VariantClear(&to);
        }
        if (old.enabled != e.enabled) {
            SetBool(&from, old.enabled); SetBool(&to, e.enabled);
            UiaRaiseAutomationPropertyChangedEvent(p.Get(), UIA_IsEnabledPropertyId, from, to);
        }
        if (old.on != e.on) {
            if (e.kind == Kind::Toggle) {
                from.vt = VT_I4; from.lVal = old.on ? ToggleState_On : ToggleState_Off;
                to.vt   = VT_I4; to.lVal   = e.on   ? ToggleState_On : ToggleState_Off;
                UiaRaiseAutomationPropertyChangedEvent(p.Get(), UIA_ToggleToggleStatePropertyId, from, to);
            } else if (e.kind == Kind::Radio) {
                SetBool(&from, old.on); SetBool(&to, e.on);
                UiaRaiseAutomationPropertyChangedEvent(p.Get(), UIA_SelectionItemIsSelectedPropertyId, from, to);
            }
        }
    }

    // Focus moving is what makes a screen reader speak the new element.
    if (d.tree.focusId != prev.focusId && d.tree.focusId >= 0 && d.Find(d.tree.focusId)) {
        if (auto p = simple(d.tree.focusId))
            UiaRaiseAutomationEvent(p.Get(), UIA_AutomationFocusChangedEventId);
    }
}
