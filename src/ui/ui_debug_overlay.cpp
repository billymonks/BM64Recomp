#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "recomp_ui.h"

#include "elements/ui_element.h"
#include "elements/ui_label.h"

namespace {
    constexpr size_t max_lines = 18;
    constexpr uint64_t draw_report_interval = 600;

    struct {
        recompui::ContextId context = recompui::ContextId::null();
        std::vector<recompui::Label*> lines;
        std::vector<std::string> shown_text;
        std::mutex mutex;
    } overlay;

    // Only touched by the UI render thread.
    uint64_t frames_drawn = 0;
}

void recompui::init_debug_overlay() {
    ContextId context = create_context();
    std::lock_guard lock{ overlay.mutex };

    context.open();
    context.set_captures_input(false);
    context.set_captures_mouse(false);

    Element* panel = context.create_element<Element>(context.get_root_element());
    panel->set_display(Display::Flex);
    panel->set_flex_direction(FlexDirection::Column);
    panel->set_position(Position::Absolute);
    panel->set_top(16, Unit::Dp);
    panel->set_left(16, Unit::Dp);
    panel->set_padding(12, Unit::Dp);
    panel->set_border_radius(8, Unit::Dp);
    panel->set_background_color(Color{ 8, 7, 13, 191 });

    for (size_t i = 0; i < max_lines; i++) {
        Label* line = context.create_element<Label>(panel, "", LabelStyle::Annotation);
        line->set_color(Color{ 242, 242, 242, 255 });
        overlay.lines.push_back(line);
    }
    overlay.shown_text.resize(max_lines);

    context.close();
    overlay.context = context;
    printf("[bm64 debug] Debug overlay created (document %p)\n", static_cast<void*>(context.get_document()));
}

void recompui::set_debug_overlay_shown(bool shown) {
    if (overlay.context == ContextId::null()) {
        printf("[bm64 debug] Debug overlay not created yet; can't %s it\n", shown ? "show" : "hide");
        return;
    }
    if (shown && !is_context_shown(overlay.context)) {
        show_context(overlay.context, "");
        printf("[bm64 debug] Debug overlay shown\n");
    }
    else if (!shown && is_context_shown(overlay.context)) {
        hide_context(overlay.context);
        printf("[bm64 debug] Debug overlay hidden\n");
    }
}

void recompui::set_debug_overlay_lines(const std::vector<std::string>& lines) {
    std::lock_guard lock{ overlay.mutex };
    if (overlay.context == ContextId::null()) {
        return;
    }

    bool opened = overlay.context.open_if_not_already();
    for (size_t i = 0; i < overlay.lines.size(); i++) {
        const std::string& text = i < lines.size() ? lines[i] : std::string{};
        if (text != overlay.shown_text[i]) {
            overlay.lines[i]->set_text(text);
            overlay.shown_text[i] = text;
        }
    }
    if (opened) {
        overlay.context.close();
    }
}

// Called by the UI draw hook after the RmlUi context has rendered. Logs evidence that the overlay is actually drawn:
// its visibility, whether it's the frontmost visible document, the panel's laid-out box and its first line of text.
void recompui::note_debug_overlay_drawn(Rml::Context* context) {
    if (overlay.context == ContextId::null()) {
        return;
    }
    Rml::ElementDocument* doc = overlay.context.get_document();
    if (doc == nullptr || !doc->IsVisible()) {
        return;
    }
    frames_drawn++;
    if (frames_drawn != 1 && (frames_drawn % draw_report_interval) != 0) {
        return;
    }

    std::string above;
    for (int i = 0; i < context->GetNumDocuments(); i++) {
        Rml::ElementDocument* other = context->GetDocument(i);
        // The RmlUi debugger's input hook is an empty document that is always visible.
        if (other != doc && other->IsVisible() && other->GetZIndex() > doc->GetZIndex() && other->GetId() != "rmlui-debug-hook") {
            above += " [" + other->GetSourceURL() + " id '" + other->GetId() + "' z " + std::to_string(other->GetZIndex()) + "]";
        }
    }
    Rml::Element* panel = doc->GetFirstChild();
    Rml::Element* line = panel ? panel->GetFirstChild() : nullptr;
    Rml::Vector2f size = panel ? panel->GetBox().GetSize(Rml::BoxArea::Border) : Rml::Vector2f{};
    Rml::Vector2f pos = panel ? panel->GetAbsoluteOffset(Rml::BoxArea::Border) : Rml::Vector2f{};
    printf("[bm64 debug] Debug overlay drawn %llu frames: z %d, visible documents above:%s, panel %.0fx%.0f at (%.0f, %.0f) in %dx%d, line 0 \"%s\"\n",
        (unsigned long long)frames_drawn, doc->GetZIndex(), above.empty() ? " none" : above.c_str(), size.x, size.y, pos.x, pos.y,
        context->GetDimensions().x, context->GetDimensions().y, line ? line->GetInnerRML().c_str() : "");
    fflush(stdout);
}
