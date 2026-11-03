#pragma once
#include <Windows.h>
#include <memory>
#include <string>
#include <vector>

// Accessibility for the tray panel, through UI Automation.
//
// The panel is drawn by hand: no child windows, no controls Windows knows about.
// To a screen reader that is one blank rectangle. This module is what describes
// the picture — what each thing is, what it is called, what state it is in, where
// it sits — and lets a screen reader operate it.
//
// It knows nothing about the panel's own types. The panel hands it a description
// (A11yTree) every time something changes, and it answers a screen reader's
// questions from the latest one. That keeps the two from depending on each other,
// and means the description is the only thing to keep in step with what is drawn.

struct A11yElement {
    enum class Kind {
        Text,    // something to read: the status line
        Button,  // does one thing when invoked
        Toggle,  // a switch: on or off. Exposed as a check box, which is what
                 // screen readers have the best vocabulary for
        Radio,   // one of a set of which exactly one is chosen
        Group,   // names a set of the above
    };

    int          id = 0;      // stable across updates, so a screen reader's hold on it survives
    int          parent = -1; // the parent's id; -1 for the panel itself
    Kind         kind = Kind::Text;
    std::wstring name;
    std::wstring help;        // the longer explanation, read after the name
    std::wstring automationId;
    bool         enabled = true;
    bool         on = false;  // Toggle: switched on. Radio: the chosen one.
    int          position = 0, size = 0;  // Radio: place in its group, 1-based
    // Client coordinates, in the panel's own units (DIPs).
    float        left = 0, top = 0, right = 0, bottom = 0;
};

struct A11yTree {
    std::wstring             name;      // the panel's own
    std::vector<A11yElement> elements;  // reading order; a parent precedes its children
    int                      focusId = -1;  // the element with keyboard focus, if any
};

class PanelAccessibility {
public:
    // What the panel offers back: where it is, and the two things a screen reader
    // can do to it. Called on the panel's own thread.
    class Host {
    public:
        virtual ~Host() = default;
        virtual HWND  Window() const = 0;
        virtual float Scale() const = 0;           // pixels per unit of A11yTree coordinates
        virtual void  Invoke(int id) = 0;          // activate: press, toggle or select
        virtual void  SetFocus(int id) = 0;
    };

    explicit PanelAccessibility(Host& host);
    ~PanelAccessibility();
    PanelAccessibility(const PanelAccessibility&) = delete;
    PanelAccessibility& operator=(const PanelAccessibility&) = delete;

    // True while any assistive technology is connected. Used to decide whether it
    // is worth doing things nobody else would see, such as putting keyboard focus
    // somewhere as soon as the panel opens.
    static bool ClientsListening();

    // Whether a WM_GETOBJECT with this lParam is asking for the UI Automation root.
    static bool WantsRoot(LPARAM lp);
    // The answer to such a message.
    LRESULT HandleGetObject(WPARAM wp, LPARAM lp);

    // Replaces what is described, and tells any listener what changed: values that
    // differ, elements that came or went, and where focus moved.
    void Update(A11yTree tree);

    // The window is going away. Providers are disconnected and answer "gone" from
    // now on; a screen reader may hold them a while after the window has closed.
    void Detach();

    struct Impl;  // opaque; named here only so the provider classes can reach it
private:
    std::unique_ptr<Impl> m_d;
};
