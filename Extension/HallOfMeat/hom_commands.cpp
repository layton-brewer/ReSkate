#include "hall_of_meat_hud.h"
#include "Extension/Console/commands.h"

// The `hom` console commands: start, stop and inspect a Hall Of Meat challenge.
namespace dingosdk::console {
void register_hall_of_meat_commands(Commands &registry) {
    struct Verb {
        const char *name, *description, *usage;
    };
    const Verb verbs[]{
        {"start", "Start a timed Hall Of Meat challenge", "[number|name]"},
        {"stop", "Stop the Hall Of Meat challenge", nullptr},
        {"list", "List the Hall Of Meat challenges", nullptr},
        {"status", "How the Hall Of Meat challenge is going", nullptr},
    };
    for (const auto &verb : verbs) {
        std::vector<Argument> arguments;
        if (verb.usage) {
            auto rest = argument(verb.usage, Type::text, true);
            rest.rest = true;
            arguments.push_back(std::move(rest));
        }
        auto entry = action(std::string("hom ") + verb.name, verb.description, Group::gameplay, std::move(arguments));
        entry.run = [name = std::string(verb.name)](const Model &, const Values &values, const Output &out) {
            std::vector<std::string> words;
            if (!values.empty())
                if (const auto *text = std::get_if<std::string>(&values[0])) {
                    std::size_t start = 0;
                    while (start < text->size()) {
                        auto end = text->find(' ', start);
                        if (end == std::string::npos) end = text->size();
                        if (end > start) words.push_back(text->substr(start, end - start));
                        start = end + 1;
                    }
                }
            out(overlay::hall_of_meat_command(name, words));
        };
        registry.add(std::move(entry));
    }
}
} // namespace dingosdk::console
