#include <array>
#include <string>

#include "librecomp/helpers.hpp"
#include "recomp_input.h"
#include "ultramodern/ultramodern.hpp"

// Arrays that hold the mappings for every input for keyboard and controller respectively.
// Controller bindings are per N64 port. Keyboard bindings are only read for port 0.
using input_mapping = std::array<recomp::InputField, recomp::bindings_per_input>;
using input_mapping_array = std::array<input_mapping, static_cast<size_t>(recomp::GameInput::COUNT)>;
static input_mapping_array keyboard_input_mappings{};
static std::array<input_mapping_array, recomp::num_controller_ports> controller_input_mappings{};

// Make the button value array, which maps a button index to its bit field.
#define DEFINE_INPUT(name, value, readable) uint16_t(value##u),
static const std::array n64_button_values = {
    DEFINE_N64_BUTTON_INPUTS()
};
#undef DEFINE_INPUT

// Make the input name array.
#define DEFINE_INPUT(name, value, readable) readable,
static const std::vector<std::string> input_names = {
    DEFINE_ALL_INPUTS()
};
#undef DEFINE_INPUT

// Make the input enum name array.
#define DEFINE_INPUT(name, value, readable) #name,
static const std::vector<std::string> input_enum_names = {
    DEFINE_ALL_INPUTS()
};
#undef DEFINE_INPUT

size_t recomp::get_num_inputs() {
    return (size_t)GameInput::COUNT;
}

const std::string& recomp::get_input_name(GameInput input) {
    return input_names.at(static_cast<size_t>(input));
}

const std::string& recomp::get_input_enum_name(GameInput input) {
    return input_enum_names.at(static_cast<size_t>(input));
}

recomp::GameInput recomp::get_input_from_enum_name(const std::string_view enum_name) {
    auto find_it = std::find(input_enum_names.begin(), input_enum_names.end(), enum_name);
    if (find_it == input_enum_names.end()) {
        return recomp::GameInput::COUNT;
    }

    return static_cast<recomp::GameInput>(find_it - input_enum_names.begin());
}

static int clamp_port(int port) {
    if (port < 0 || port >= recomp::num_controller_ports) {
        return 0;
    }
    return port;
}

static input_mapping_array& mappings_for(recomp::InputDevice device, int port) {
    if (device == recomp::InputDevice::Controller) {
        return controller_input_mappings[clamp_port(port)];
    }
    return keyboard_input_mappings;
}

// Due to an RmlUi limitation this can't be const. Ideally it would return a const reference or even just a straight up copy.
recomp::InputField& recomp::get_input_binding(GameInput input, size_t binding_index, recomp::InputDevice device, int port) {
    input_mapping& cur_input_mapping = mappings_for(device, port).at(static_cast<size_t>(input));

    if (binding_index < cur_input_mapping.size()) {
        return cur_input_mapping[binding_index];
    }
    else {
        static recomp::InputField dummy_field = {};
        return dummy_field;
    }
}

void recomp::set_input_binding(recomp::GameInput input, size_t binding_index, recomp::InputDevice device, recomp::InputField value, int port) {
    input_mapping& cur_input_mapping = mappings_for(device, port).at(static_cast<size_t>(input));

    if (binding_index < cur_input_mapping.size()) {
        cur_input_mapping[binding_index] = value;
    }
}

std::string recomp::joined_controller_binding_text(GameInput input) {
    std::string joined;
    for (int port = 0; port < num_controller_ports; port++) {
        for (size_t binding_index = 0; binding_index < bindings_per_input; binding_index++) {
            std::string text = get_input_binding(input, binding_index, InputDevice::Controller, port).to_string();
            if (text.empty() || joined.find(text) != std::string::npos) {
                continue;
            }
            joined += text;
        }
    }
    return joined;
}

static bool mapping_held(const input_mapping& mapping, int port) {
    bool held = false;
    for (const recomp::InputField& field : mapping) {
        held |= recomp::get_input_digital(field, port);
    }
    return held;
}

static float mapping_axis(const input_mapping& mapping, int port) {
    float value = 0.0f;
    for (const recomp::InputField& field : mapping) {
        value += recomp::get_input_analog(field, port);
    }
    return std::clamp(value, 0.0f, 1.0f);
}

bool recomp::get_n64_input(int controller_num, uint16_t* buttons_out, float* x_out, float* y_out) {
    if (controller_num < 0 || controller_num >= num_controller_ports || !port_is_connected(controller_num)) {
        return false;
    }

    uint16_t cur_buttons = 0;
    float cur_x = 0.0f;
    float cur_y = 0.0f;

    if (!recomp::game_input_disabled()) {
        const input_mapping_array& port_mappings = controller_input_mappings[controller_num];
        for (size_t i = 0; i < n64_button_values.size(); i++) {
            size_t input_index = (size_t)GameInput::N64_BUTTON_START + i;
            if (controller_num == 0) {
                cur_buttons |= mapping_held(keyboard_input_mappings[input_index], controller_num) ? n64_button_values[i] : 0;
            }
            cur_buttons |= mapping_held(port_mappings[input_index], controller_num) ? n64_button_values[i] : 0;
        }

        float joystick_x = mapping_axis(port_mappings[(size_t)GameInput::X_AXIS_POS], controller_num)
                        - mapping_axis(port_mappings[(size_t)GameInput::X_AXIS_NEG], controller_num);

        float joystick_y = mapping_axis(port_mappings[(size_t)GameInput::Y_AXIS_POS], controller_num)
                        - mapping_axis(port_mappings[(size_t)GameInput::Y_AXIS_NEG], controller_num);

        recomp::apply_joystick_deadzone(joystick_x, joystick_y, &joystick_x, &joystick_y);

        cur_x = joystick_x;
        cur_y = joystick_y;
        if (controller_num == 0) {
            cur_x += mapping_axis(keyboard_input_mappings[(size_t)GameInput::X_AXIS_POS], controller_num)
                    - mapping_axis(keyboard_input_mappings[(size_t)GameInput::X_AXIS_NEG], controller_num);
            cur_y += mapping_axis(keyboard_input_mappings[(size_t)GameInput::Y_AXIS_POS], controller_num)
                    - mapping_axis(keyboard_input_mappings[(size_t)GameInput::Y_AXIS_NEG], controller_num);
        }
    }

    *buttons_out = cur_buttons;
    *x_out = std::clamp(cur_x * 0.65f, -1.0f, 1.0f);
    *y_out = std::clamp(cur_y * 0.65f, -1.0f, 1.0f);

    return true;
}
