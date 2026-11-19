
#include <caf/actor_system.hpp>
#include <caf/caf_main.hpp>
#include <caf/event_based_actor.hpp>

#include <string>

using namespace caf;
using namespace std::literals;

behavior mirror(event_based_actor *self) {
    return {
        [self](const std::string &text) -> std::string {
            self->println("{}", text);
            return std::string{text.rbegin(), text.rend()};
        },
    };
}

void hello_world(event_based_actor *self, const actor &buddy) {
    self->mail("Hello World!")
        .request(buddy, 10s)
        .then([self](const std::string &text) { self->println("{}", text); });
}

void caf_main(actor_system &system) {
    auto mirror_actor = system.spawn(mirror);
    system.spawn(hello_world, mirror_actor);
}

CAF_MAIN()
