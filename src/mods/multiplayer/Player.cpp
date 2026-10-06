#include <sdk/EntityList.hpp>
#include <sdk/Entity.hpp>
#include "Player.hpp"

sdk::Pl0000* Player::get_entity() {
    if (!m_entity_handle) {
        return nullptr;
    }

    auto ent = sdk::EntityList::get()->get_by_handle(m_entity_handle);

    // The handle may belong to something that isn't an android (Flight Unit...).
    if (!ent || ent->behavior == nullptr || !ent->behavior->is_pl0000()) {
        return nullptr;
    }

    return ent->behavior->as<sdk::Pl0000>();
}
