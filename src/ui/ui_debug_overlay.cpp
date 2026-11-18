#include <mutex>
#include <string>
#include <vector>

#include "recomp_ui.h"

#include "elements/ui_element.h"
#include "elements/ui_label.h"

namespace {
    constexpr size_t max_lines = 12;

    struct {
        recompui::ContextId context = recompui::ContextId::null();
        std::vector<recompui::Label*> lines;
        std::vector<std::string> shown_text;
        std::mutex mutex;
    } overlay;
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
}

void recompui::set_debug_overlay_shown(bool shown) {
    if (overlay.context == ContextId::null()) {
        return;
    }
    if (shown && !is_context_shown(overlay.context)) {
        show_context(overlay.context, "");
    }
    else if (!shown && is_context_shown(overlay.context)) {
        hide_context(overlay.context);
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
