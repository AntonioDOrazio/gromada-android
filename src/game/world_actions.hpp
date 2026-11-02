#pragma once
// The 21 Action() functions of gromada.exe (docs/re/engine.md §2.3, §3.4, §5, §7, §8).
// Included at the end of world.hpp. Each class handles some actions and passes the others to
// its base: class -> material (0x412580) -> sprite (0x40d9e0).

namespace gromada::game {

inline int World::action(Sprite& s, int a, int v1, int v2) {
    if (!s.alive) return 0;
    if (actionHook) {
        int ret = 0;
        if (actionHook(s, a, v1, v2, ret)) return ret;
    }
    switch (s.cls) {
        case cls::Terrain:
        case cls::Static: return actionMaterial(s, a, v1, v2);
        case cls::Vehicle: return actionVehicle(s, a, v1, v2);
        case cls::Building: return actionBuilding(s, a, v1, v2);
        case cls::Avia: return actionAvia(s, a, v1, v2);
        case cls::Projectile: return actionProjectile(s, a, v1, v2);
        case cls::CannonMissile: return actionCannonMissile(s, a, v1, v2);
        case cls::DownedAvia: return actionDownedAvia(s, a, v1, v2);
        case cls::NA: return actionNA(s, a, v1, v2);
        case cls::Superstructure: return actionBase(s, a, v1, v2);
        case cls::Image: return actionImage(s, a, v1, v2);
        case cls::Effect1: return actionEffect1(s, a, v1, v2);
        case cls::Effect2: return actionEffect2(s, a, v1, v2);
        case cls::Mine: return actionMine(s, a, v1, v2);
        case cls::Shell: return actionShell(s, a, v1, v2);
        case cls::AviaMissile: return actionAviaMissile(s, a, v1, v2);
        case cls::Debris: return actionDebris(s, a, v1, v2);
        case cls::Kassandra: return actionKassandra(s, a, v1, v2);
        case cls::Bonus: return actionBonus(s, a, v1, v2);
        case cls::Font: return actionFont(s, a, v1, v2);
        case cls::RepairCannon: return actionRepairCannon(s, a, v1, v2);
        default: return actionBase(s, a, v1, v2);
    }
}

// 0x40d9e0: actions common to every sprite.
inline int World::actionBase(Sprite& s, int a, int v1, int v2) {
    switch (a) {
        case act::changeaction: changeAction(s, v1, v2); return 0;
        case act::nextcommand:
            if (s.action == act::death) return 0;
            if ((s.state & st::Move) || s.speed != 0) changeAction(s, act::go, -1);
            else if (s.action != act::stand_open) changeAction(s, act::stand, -1);
            return 0;
        case act::destroy: destroyBase(s); return 0;
        case act::changevid:
            if (v1 < 0 || v1 >= static_cast<int>(m_vids.size())) return 0;
            s.nvid = static_cast<std::uint16_t>(v1);
            if (v2 >= 0) changeAction(s, v2, -1); else setAnimation(s);
            return 0;
        case act::clearcom: s.commands.reset(); return 0;
        case act::changecoor:
            if (vidOf(s).flags & vf::PresentOnGrid) gridMoveTo(s, v1, v2);
            return 0;
        case act::getlink: return idOf(s.child);
        case act::getarmy: return vidOf(s).army;
        case act::setstate: s.state = static_cast<std::uint16_t>(v1); return 0;
        case act::getstate: return s.state;
        case act::getaction: return s.action;
        default: return 0;
    }
}

// 0x412580: "material" objects (hp, damage, death, area damage).
inline int World::actionMaterial(Sprite& s, int a, int v1, int v2) {
    const VidInfo& v = vidOf(s);
    switch (a) {
        case act::l_rotate:
        case act::r_rotate:
        case act::setarmy:
        case act::setbuildtime: return 0;
        case act::death: {   // area damage at the end of the death animation
            if (!v.deathDamage) return 0;
            const int rx = v.halfX + v.deathDamageRadius, ry = v.halfY + v.deathDamageRadius;
            s.hp = 0;
            const auto victims = collectInRect(s.x - rx, s.y - ry, s.x + rx, s.y + ry);
            for (Sprite* o : victims) {
                if (o == &s || !o->alive) continue;
                const VidInfo& ov = vidOf(*o);
                if (!ov.maxHP) continue;
                if (std::abs(s.x - o->x) >= ov.halfX + rx || std::abs(s.y - o->y) >= ov.halfY + ry ||
                    std::abs(s.z - o->z) >= ov.absSizeZ + 20)
                    continue;
                action(*o, act::damage, v.deathDamage, s.id);
            }
            return 0;
        }
        case act::path_block: changeAction(s, act::stand, -1); return 0;
        case act::changeaction: changeAction(s, v1, v2); return 0;
        case act::path_limit:
            s.x = static_cast<std::int16_t>(s.x + v1);
            s.y = static_cast<std::int16_t>(s.y + v2);
            changeAction(s, act::death, -1);
            return 0;
        case act::drawselected: return 0;
        case act::damage: {
            if (damageHook) damageHook(s, v1, v2);
            if (!v.maxHP) return 0;
            const int half = v.maxHP >> 1;
            const bool wasAbove = half < s.hp;
            if (v1 >= s.hp) { changeAction(s, act::death, -1); return 0; }
            s.hp = static_cast<std::uint8_t>(s.hp - v1);
            if (s.hp > half && !wasAbove) removeChildNvid(s, v.childNvid[act::wound]);
            if (s.hp <= (vidOf(s).maxHP >> 1) && wasAbove) changeAction(s, act::wound, -1);
            return 0;
        }
        case act::init: s.hp = v.maxHP; return 0;
        case act::save: return 0;
        case act::restore: return 0;
        case act::destroy:
            if (!(s.state & (st::Screen | st::Sub)) && (v.flags & vf::PresentOnGrid)) {
                const UnitId me = s.id;
                const auto units = m_gridUnits;   // the callee may alter the list
                for (Sprite* u : units)
                    if (u->alive) action(*u, act::deletetarget, me, 0);
            }
            return actionBase(s, a, v1, v2);
        case act::gethp: return s.hp;
        case act::getarmy: return v.army;
        case act::getbuildtime: return 1;
        default: return actionBase(s, a, v1, v2);
    }
}

inline void World::removeAttackCommands(Sprite& s, UnitId t) {
    if (!s.commands) return;
    for (int i = s.commands->count - 1; i >= 0 && s.commands; --i)
        if (s.commands->e[static_cast<std::size_t>(i)].act == act::attack && s.commands->e[static_cast<std::size_t>(i)].var1 == t)
            removeCommand(s, i);
}

inline void World::unitDestroyCommon(Sprite& s) {
    removeFromGroups(&s);
    for (auto& p : m_players) {
        if (p.flagman == s.id) p.flagman = 0;
        if (p.underCursor == s.id) p.underCursor = 0;
    }
}

// 0x40ea20: ground vehicles (also the base of Building, Avia, Mine, Kassandra).
inline int World::actionVehicle(Sprite& s, int a, int v1, int v2) {
    const VidInfo& v = vidOf(s);
    switch (a) {
        case act::stand:
        case act::go:
        case act::fight:
        case act::select: return 0;
        case act::setarmy: s.army = static_cast<std::uint8_t>(v1); return 0;
        case act::getarmy: return s.army;
        case act::setbuildtime: s.buildTime = static_cast<std::uint8_t>(v1); return 0;
        case act::getbuildtime: return s.buildTime;
        case act::setbehave: s.behave = static_cast<std::uint8_t>(v1); return 0;
        case act::getbehave: return s.behave;
        case act::gettarget: return byId(s.target) ? s.target : 0;
        case act::coor_attack:
            s.attackX = static_cast<std::int16_t>(v1);
            s.state |= st::Attack;
            s.target = 0;
            if (weaponOf(s).flags & 8) { s.attackZ = 80; s.attackY = static_cast<std::int16_t>(v2 + 80); }
            else { s.attackZ = 19; s.attackY = static_cast<std::int16_t>(v2); }
            return 0;
        case act::additem: addItem(s, v1); return 0;
        case act::askitem: return askItem(s, v1);
        case act::deleteitem: return deleteItem(s, v1);
        case act::deleteallitem: s.items.clear(); return 0;
        case act::death: {   // 0x40eb62: Bonus items are thrown in the air
            const auto items = s.items;
            for (auto it : items) {
                if (it < 0 || it >= static_cast<int>(m_vids.size()) || m_vids[static_cast<std::size_t>(it)].cls != cls::Bonus) continue;
                const int dir = rand() & 0xff;
                if (Sprite* c = createUnit(it, s.x, s.y, s.z + 10, 0, dir, nullptr)) {
                    c->state |= st::Accelerate;
                    c->goZ = 0;
                    c->vz = vidOf(*c).speedY;
                }
            }
            return actionMaterial(s, a, v1, v2);
        }
        case act::destroy:
            s.items.clear();
            unitDestroyCommon(s);
            return actionMaterial(s, a, v1, v2);
        case act::init:
            actionMaterial(s, a, v1, v2);
            s.wpX = s.wpY = -1;
            s.target = 0;
            s.behave = 1;
            s.turn = 0;
            s.buildTime = 20;
            s.army = v.army;
            return 0;
        case act::deletetarget:
            removeAttackCommands(s, v1);
            if (s.target == v1) { s.state &= ~st::Attack; s.target = 0; }
            return 0;
        case act::nextcommand: vehicleNextCommand(s); return 0;
        case act::damage: {
            if (damageHook) damageHook(s, v1, v2);
            if (s.action == act::unload || !v.maxHP) return 0;
            const int half = v.maxHP >> 1;
            const bool wasAbove = half < s.hp;
            if (v1 >= s.hp) { clearWaypoint(s); changeAction(s, act::death, -1); return 0; }
            s.hp = static_cast<std::uint8_t>(s.hp - v1);
            if (s.hp > half && !wasAbove) removeChildNvid(s, v.childNvid[act::wound]);
            if (s.hp <= (vidOf(s).maxHP >> 1) && wasAbove) { changeAction(s, act::wound, -1); return 0; }
            if (!(s.state & st::CommandMask) && vidOf(s).speedX && (s.behave & 4)) {   // evasive
                const int d = rand() & 0xff;
                approach(s, s.x + (fx::Sin[d] >> 4), s.y + (fx::NegCos[d] >> 4));
            }
            return 0;
        }
        case act::attack: {
            Sprite* t = byId(v1);
            if (!t || !v.weapon || t == &s) return 0;
            clearWaypoint(s);
            s.state |= st::Attack;
            s.target = v1;
            return 0;
        }
        case act::move:
            if (!v.speedX) return 0;
            s.state &= ~st::Attack;
            s.target = 0;
            approach(s, v1, v2);
            return 0;
        case act::submove:
            if (v.speedX) approach(s, v1, v2);
            return 0;
        case act::jump: {
            if (s.z != 0 || !v.speedX) return 0;
            approach(s, v1, v2);
            s.state |= st::Jump;
            const auto d = getDirection(s.x, s.y, v1, v2);
            s.direction = d.dir();
            changeAction(s, act::fight, d.dir());
            s.speed = static_cast<std::int16_t>(v.speedX + (v.speedX >> 1));
            s.vz = s.speed ? static_cast<std::int16_t>(((m_lastDist * fx::Gravity) / s.speed) >> 1) : std::int16_t{0};
            return 0;
        }
        case act::path_block: {
            if (s.vz) {
                if (s.state & st::Jump) { s.state &= ~st::Jump; s.speed = 0; }
                s.vz = 0;
                return 0;
            }
            s.speed = static_cast<std::int16_t>(s.speed >> 1);
            s.direction = static_cast<std::uint8_t>(s.direction + 0x80);
            if (s.turn) return 0;
            const auto ox = s.x, oy = s.y;
            s.turn = 30;
            for (int i = 0; i < 4; ++i) {
                const std::uint8_t d = static_cast<std::uint8_t>(s.direction - 0x40);
                s.x = static_cast<std::int16_t>(s.x + (fx::Sin[d] >> 5));
                s.y = static_cast<std::int16_t>(s.y + (fx::NegCos[d] >> 5));
                if (!firstObstacleOnLine(s, s.wpX, s.wpY, s.z)) { s.turn = -30; break; }
            }
            s.x = ox; s.y = oy;
            return 0;
        }
        case act::path_limit: clearWaypoint(s); return 0;
        case act::save:
        case act::restore: return 0;
        default:
            if (a > 0x51) return actionMaterial(s, a, v1, v2);
            return actionMaterial(s, a, v1, v2);
    }
}

// 0x40ed35: act_nextcommand of the ground vehicles (waypoints, steering, targets, fire).
inline void World::vehicleNextCommand(Sprite& s) {
    if (s.action == act::death || s.action == act::unload) return;
    if (s.speed < 0) {   // reversing (player): resume towards the waypoint when aligned and free
        const int dir = s.direction;
        const int d = getDirection(s.x, s.y, s.wpX, s.wpY).dir();
        if ((std::abs(d - dir) & 0xff) <= 16 &&
            !askPlace(s, s.x + (fx::Sin[dir] >> 5), s.y + (fx::NegCos[dir] >> 5), s.z)) {
            setDestination(s, s.wpX, s.wpY, s.z);
            return;
        }
    }
    if ((s.state & 3) == 3) stop(s);
    if (s.wpX >= 0) {
        if (std::abs(s.wpX - s.x) <= m_scaleX && std::abs(s.wpY - s.y) <= m_scaleY) clearWaypoint(s);
        else if (!(s.state & st::Move)) setDestination(s, s.wpX, s.wpY, s.z);
    } else {
        changeAction(s, act::stand, -1);
    }
    if (s.state & st::Move) {
        std::uint8_t d;
        if (s.turn) {
            d = static_cast<std::uint8_t>(s.direction + s.turn);
            if (s.turn < 0) s.turn++; else s.turn--;
        } else {
            d = getDirection(s.x, s.y, s.goX, s.goY).dir();
        }
        if (s.speed < 0) d = static_cast<std::uint8_t>(d + 0x80);
        if (!turnTo(s, d)) s.frame = static_cast<std::int16_t>(s.lastFrame + 1);
        else if (s.action != act::go) changeAction(s, act::go, -1);
    }
    const VidInfo& v = vidOf(s);
    if (v.weapon && (s.behave & 1)) {
        const auto cmd = s.state & st::CommandMask;
        if (cmd == 0 || cmd == st::Move) {
            Sprite* t = findTarget(s);
            s.target = idOf(t);
            if (t && !(s.behave & 4)) action(s, act::attack, t->id, 0);
        }
    }
    const Weapon& w = weaponOf(s);
    const int range = static_cast<std::int16_t>(w.weaponRange);
    if (Sprite* t = byId(s.target)) {
        if (distance(s, *t) > range) {
            if ((s.state & st::Attack) && !(s.state & st::Move)) approach(s, t->x, t->y);
            else if (vidOf(s).linkedObjectVid && s.child) turnTo(*s.child, s.direction);
            return;
        }
        if ((s.state & st::Attack) && (s.state & st::Move)) clearWaypoint(s);
        const auto tsz = vidOf(*t).sizeZ;
        fire(s, t->x, t->y, t->z - (tsz >> 2) + tsz);
        return;
    }
    s.target = 0;
    if (s.state & st::Attack) {
        getDirection(s.x, s.y, s.attackX, s.attackY);
        if (m_lastDist <= range) {
            if (fire(s, s.attackX, s.attackY, s.attackZ)) {
                s.state &= ~st::Attack;
                stop(s);
                s.wpX = s.wpY = -1;
            }
        } else if (!(s.state & st::Move)) {
            approach(s, s.attackX, s.attackY);
        }
        return;
    }
    if (w.flags & 2) {   // radar / idle turret: one step counter-clockwise
        const std::uint8_t d = fx::rotateStep(vidOf(s).dirs, s.direction, false);
        if (!vidOf(s).linkedObjectVid) rotateChain(s, d);
        else if (s.child) changeAction(*s.child, act::l_rotate, d);
        return;
    }
    if (vidOf(s).linkedObjectVid && s.child) turnTo(*s.child, s.direction);
}

// 0x410e10: factories and buildings.
inline int World::actionBuilding(Sprite& s, int a, int v1, int v2) {
    const VidInfo& v = vidOf(s);
    switch (a) {
        case act::uncom:
            pushCommandTop(s, a, v1, v2);
            action(s, act::nextcommand, 0, 0);
            return 0;
        case act::calltact: {   // raise/lower the unit being produced
            Sprite* cargo = byId(s.target);
            if (s.action == act::unload && cargo) {
                if (s.frame <= s.lastFrame && cargo->z < v.sizeZ)
                    cargo->z = static_cast<std::int16_t>(cargo->z + (v.sizeZ - cargo->z) / (s.lastFrame - s.frame + 1));
            } else if (s.action == act::load && cargo && s.frame <= s.lastFrame) {
                if (cargo->z > v.sizeZ - 85)
                    cargo->z = static_cast<std::int16_t>(cargo->z + (v.sizeZ - cargo->z - 85) / (s.lastFrame - s.frame + 1));
            }
            return 0;
        }
        case act::load_object: {
            Sprite* u = byId(v1);
            if (s.action != act::stand_open || byId(s.target) || !u) return 0;
            s.target = v1;
            u->state = static_cast<std::uint16_t>((u->state & ~st::Accelerate) | st::Passive);
            u->speed = u->vz = 0;
            pushCommandTop(s, act::close, 0, 0);
            pushCommandTop(s, act::load, 0, 0);
            return 0;
        }
        case act::unload_object: {
            Sprite* u = byId(v1);
            if (s.action != act::stand || byId(s.target) || !u) return 0;
            s.target = v1;
            u->state = static_cast<std::uint16_t>((u->state & ~st::Accelerate) | st::Passive);
            u->speed = u->vz = 0;
            changeAction(*u, act::unload, -1);
            if (!v.animLen[act::open]) u->state |= st::Invisible;
            pushCommandTop(s, act::stand_open, 0, 0);
            pushCommandTop(s, act::unload, 0, 0);
            changeAction(s, act::open, -1);
            return 0;
        }
        case act::buildunit:
            if (s.timer || v1 < 0 || v1 >= static_cast<int>(m_vids.size())) return 0;
            s.buildNvid = static_cast<std::uint16_t>(v1);
            s.state |= st::Constructing;
            s.timer = static_cast<std::uint16_t>(s.buildTime * 1000);
            return 0;
        case act::damage:
            if (s.action == act::unload) return 0;
            return actionVehicle(s, a, v1, v2);
        case act::nextcommand: {
            if (s.action == act::death) return 0;
            if ((s.state & 3) == 3) stop(s);
            if (s.state & st::Move) {
                std::uint8_t d = getDirection(s.x, s.y, s.goX, s.goY).dir();
                if (s.speed < 0) d = static_cast<std::uint8_t>(d + 0x80);
                if (!turnTo(s, d)) s.frame = static_cast<std::int16_t>(s.lastFrame + 1);
                else if (s.action != act::go) changeAction(s, act::go, -1);
            }
            if ((s.state & st::Constructing) && s.timer == 0) {
                const int z0 = v.animLen[act::unload] ? v.sizeZ - 95 : 0;
                s.state &= ~st::Constructing;
                const int nv = s.buildNvid;
                Sprite* b = askPlaceVid(s.x, s.y, z0, nv);
                Sprite* cargo = (b && b != &s) ? nullptr : createUnit(nv, s.x, s.y, z0, act::unload, 0x80, nullptr);
                s.target = idOf(cargo);
                if (!cargo) { changeAction(s, act::stand, -1); return 0; }
                if (s.commands)
                    for (int i = 0; s.commands && i < s.commands->count; ++i) {
                        const Command c = s.commands->e[static_cast<std::size_t>(i)];
                        if (c.act == act::uncom) break;
                        pushCommandTop(*cargo, c.act, c.var1, c.var2);
                    }
                const auto ccls = vidOf(*cargo).cls;
                if (ccls == cls::Vehicle || ccls == cls::Avia) cargo->behave = s.behave;
                if (v.animLen[act::unload]) cargo->goZ = static_cast<std::int16_t>(cargo->z + 1);
                cargo->state = static_cast<std::uint16_t>((cargo->state & ~st::Accelerate) | st::Passive);
                cargo->speed = cargo->vz = 0;
                changeAction(*cargo, act::unload, -1);
                if (!v.animLen[act::open]) cargo->state |= st::Invisible;
                pushCommandTop(s, act::stand_open, 0, 0);
                if (v.animLen[act::unload]) pushCommandTop(s, act::unload, 0, 0);
                changeAction(s, act::open, -1);
                return 0;
            }
            if (s.action == act::stand_open) {
                if (Sprite* cargo = byId(s.target)) {
                    if (!v.animLen[act::open]) cargo->state &= ~st::Invisible;
                    cargo->state &= ~st::Passive;
                    if (vidOf(*cargo).cls != cls::Kassandra) {
                        cargo->state |= st::Accelerate;
                        cargo->goZ = 80;
                        cargo->vz = vidOf(*cargo).speedY;
                        if (cargo->action == act::unload) changeAction(*cargo, act::go, -1);
                    }
                    if (cargo->x >= s.x - v.halfX && cargo->x <= s.x + v.halfX && cargo->y >= s.y - v.halfY &&
                        cargo->y <= s.y + v.halfY)
                        return 0;
                }
                s.target = 0;
                if (!(s.behave & 0x80)) {
                    pushCommandTop(s, act::close, 0, 0);
                    pushCommandTop(s, act::load, 0, 0);
                }
                return 0;
            }
            if (!(s.state & st::CommandMask) && !byId(s.target)) {
                s.target = 0;
                const int n = static_cast<int>(s.items.size());
                int cnt = n;
                for (auto it : s.items)
                    if (it >= 0 && it < static_cast<int>(m_vids.size()) && m_vids[static_cast<std::size_t>(it)].cls == cls::Bonus) --cnt;
                if (cnt) {
                    int r = rand() % (cnt + 1);
                    int i = 0;
                    for (; i < n; ++i) {
                        const auto it = s.items[static_cast<std::size_t>(i)];
                        if (it >= 0 && it < static_cast<int>(m_vids.size()) && m_vids[static_cast<std::size_t>(it)].cls == cls::Bonus) continue;
                        if (--r <= 0) break;
                    }
                    if (i < n) action(s, act::buildunit, s.items[static_cast<std::size_t>(i)], 0);
                }
                changeAction(s, act::stand, -1);
            }
            changeAction(s, act::stand, -1);
            return 0;
        }
        case act::save: return 0;
        case act::destroy:
            if (Sprite* cargo = byId(s.target)) { s.target = 0; action(*cargo, act::destroy, 0, 0); }
            return actionVehicle(s, a, v1, v2);
        default: return actionVehicle(s, a, v1, v2);
    }
}

// 0x4105a0: aircraft.
inline int World::actionAvia(Sprite& s, int a, int v1, int v2) {
    const VidInfo& v = vidOf(s);
    switch (a) {
        case act::stand:
        case act::go:
        case act::fight: return 0;
        case act::setbuildtime: s.buildTime = static_cast<std::uint8_t>(v1); return 0;
        case act::getbuildtime: return s.buildTime;
        case act::setarmy: s.army = static_cast<std::uint8_t>(v1); return 0;
        case act::getarmy: return s.army;
        case act::gettarget: return byId(s.target) ? s.target : 0;
        case act::deletetarget:
            removeAttackCommands(s, v1);
            if (s.target == v1) { s.state &= ~st::Attack; s.target = 0; }
            return 0;
        case act::child: {
            Sprite* p = byId(v1);
            if (!p) return 0;
            const auto pc = vidOf(*p).cls;
            if ((pc == cls::Vehicle || pc == cls::Building) && p->target != s.id) s.target = p->target;
            return 0;
        }
        case act::move:
            if (!v.speedX) return 0;
            s.state &= ~st::Attack;
            s.target = 0;
            setDestination(s, v1, v2, 80);
            return 0;
        case act::attack:
            if (!v1 || v1 == s.id || !byId(v1)) return 0;
            s.target = v1;
            s.state = static_cast<std::uint16_t>((s.state & ~st::Move) | st::Attack);
            return 0;
        case act::path_limit:
            s.x = static_cast<std::int16_t>(s.x + v1);
            s.y = static_cast<std::int16_t>(s.y + v2);
            if (s.x < -120 || s.y < -100 || s.x > m_mapW + 120 || s.y > m_mapH + 100) changeAction(s, act::death, -1);
            return 0;
        case act::path_block:
            s.x = static_cast<std::int16_t>(s.x + v1);
            s.y = static_cast<std::int16_t>(s.y + v2);
            return 0;
        case act::nextcommand: {
            if (s.action == act::death || s.action == act::unload) return 0;
            if ((s.state & 3) == 3) s.state &= ~st::Move;
            Sprite* t = byId(s.target);
            if (!t) s.target = 0;
            const Weapon& w = weaponOf(s);
            bool turned = false;
            if (t && s.timer == 0 && distance(s, *t) <= static_cast<std::int16_t>(w.weaponRange)) {
                changeAction(s, act::fight, -1);
                ++fireCount;
                s.timer = static_cast<std::uint16_t>(w.cooldown * 10);
                s.attackX = t->x; s.attackY = t->y; s.attackZ = t->z;
                turned = true;
            } else if (s.state & st::Move) {
                if (turnTo(s, getDirection(s.x, s.y, s.goX, s.goY).dir())) changeAction(s, act::go, -1);
                turned = true;
            } else if (t && s.timer == 0) {
                if (turnTo(s, getDirection(s.x, s.y, t->x, t->y).dir())) changeAction(s, act::go, -1);
                turned = true;
            } else if (s.z != 0) {   // free flight: probe 128 px ahead
                const std::uint8_t d = s.direction;
                if (askPlaceBounded(s, s.x + (fx::Sin[d] >> 3), s.y + (fx::NegCos[d] >> 3))) {
                    changeAction(s, act::r_rotate, fx::rotateStep(v.dirs, d, true));
                } else {
                    const int r = rand() % 5;
                    if (r == 0) {
                        if (s.action == act::r_rotate) changeAction(s, act::go, -1);
                        else changeAction(s, act::l_rotate, fx::rotateStep(v.dirs, d, false));
                    } else if (r == 1) {
                        if (s.action == act::l_rotate) changeAction(s, act::go, -1);
                        else changeAction(s, act::r_rotate, fx::rotateStep(v.dirs, d, true));
                    } else {
                        changeAction(s, act::go, -1);
                    }
                }
                turned = true;
            }
            (void)turned;
            if (!byId(s.target) && (s.behave & 1) && v.weapon && !(s.state & st::Attack)) {
                Sprite* nt = findTarget(s);
                s.target = idOf(nt);
                if (nt && !(s.behave & 4) && (!(s.state & st::Move) || (s.behave & 2)))
                    s.state = static_cast<std::uint16_t>((s.state & ~st::Move) | st::Attack);
            }
            if (s.z == 0) {
                if (!byId(s.target) && !(s.behave & 2)) return 0;
                s.state |= st::Accelerate;
                s.goZ = 80;
                s.vz = v.speedY;
                return 0;
            }
            const std::uint8_t d = s.direction;
            Sprite* b = askPlaceBounded(s, s.x + (fx::Sin[d] >> 5), s.y + (fx::NegCos[d] >> 5));
            if (b) {
                if (b == &s) { changeAction(s, act::r_rotate, fx::rotateStep(v.dirs, d, true)); return 0; }
                if (b->vz > 0) { s.goZ = static_cast<std::int16_t>(s.goZ - 20); s.vz = static_cast<std::int16_t>(-v.speedY); }
                else { s.goZ = static_cast<std::int16_t>(s.goZ + 20); s.vz = v.speedY; }
                return 0;
            }
            if (s.z < 50) { s.goZ = 80; s.vz = v.speedY; }
            else if (s.z > 120) { s.goZ = 80; s.vz = static_cast<std::int16_t>(-v.speedY); }
            return 0;
        }
        case act::init:
            s.target = 0;
            s.behave = 5;
            s.buildTime = 20;
            s.hp = v.maxHP;
            s.army = v.army;
            if (s.z) { s.state |= st::Accelerate; s.vz = v.speedY; s.goZ = 80; }
            return 0;
        case act::destroy:
            unitDestroyCommon(s);
            return actionMaterial(s, a, v1, v2);
        case act::save:
        case act::restore: return actionVehicle(s, a, v1, v2);
        case act::damage:
            if (s.action == act::unload) return 0;
            return actionMaterial(s, a, v1, v2);
        default: return actionMaterial(s, a, v1, v2);
    }
}

// 0x40f710: projectiles (go to the point fixed when fired, explode on arrival).
inline int World::actionProjectile(Sprite& s, int a, int v1, int v2) {
    const VidInfo& v = vidOf(s);
    switch (a) {
        case act::path_block:
        case act::path_limit:
            s.x = static_cast<std::int16_t>(s.x + v1);
            s.y = static_cast<std::int16_t>(s.y + v2);
            changeAction(s, act::death, -1);
            return 0;
        case act::nextcommand:
            if ((s.state & 3) == 3) { stop(s); changeAction(s, act::death, -1); }
            if (s.action == act::death) return 0;
            if (s.z <= 0) { s.goZ = 0; s.z = 0; s.vz = 0; changeAction(s, act::death, -1); return 0; }
            if (s.state & st::Move) {
                std::uint8_t d = getDirection(s.x, s.y, s.goX, s.goY).dir();
                if (s.speed < 0) d = static_cast<std::uint8_t>(d + 0x80);
                if (!turnTo(s, d)) s.frame = static_cast<std::int16_t>(s.lastFrame + 1);
                if (s.action != act::go) changeAction(s, act::go, -1);
            }
            return 0;
        case act::init:
            s.hp = v.maxHP;
            changeAction(s, act::death, -1);
            if (v.speedY) s.vz = v.speedY;
            if (v.speedX) s.state |= st::Accelerate;
            return 0;
        case act::move:
            if (!v.speedX) return 0;
            setDirection(s, getDirection(s.x, s.y, v1, v2).raw);
            setDestination(s, v1, v2, s.z);
            return 0;
        case act::attack: {
            Sprite* t = byId(v1);
            if (!v.speedX || !t) return 0;
            setDirection(s, getDirection(s.x, s.y, t->x, t->y).raw);
            int tz = t->z ? t->z : vidOf(*t).sizeZ;
            if (tz < 0) tz = 0;
            setDestination(s, t->x, t->y, tz);
            return 0;
        }
        case act::child: return 0;
        default: return actionMaterial(s, a, v1, v2);
    }
}

// 0x40f9c0: repair shot (rebuilds the wrecks in its radius).
inline int World::actionRepairCannon(Sprite& s, int a, int v1, int v2) {
    if (a == act::child) { changeAction(s, act::death, -1); return 0; }
    if (a != act::death) return actionProjectile(s, a, v1, v2);
    const int r = vidOf(s).visibilityRadius;
    const int x = s.x, y = s.y;
    const auto wrecks = collectInRect(x - r, y - r, x + r, y + r);
    for (Sprite* o : wrecks) {
        if (!o->alive) continue;
        const VidInfo& ov = vidOf(*o);
        if (x < o->x - ov.halfX || x > o->x + ov.halfX || y < o->y - ov.halfY || y > o->y + ov.halfY) continue;
        for (std::size_t i = 0; i < m_vids.size(); ++i) {
            if (m_vids[i].childNvid[act::death] != o->nvid || m_vids[i].category != cat::Vehicle) continue;
            const int ox = o->x, oy = o->y, oz = o->z, odir = o->direction;
            action(*o, act::destroy, 0, 0);
            createUnit(static_cast<int>(i), ox, oy, oz, 0, odir, nullptr);
            break;
        }
    }
    return actionMaterial(s, act::death, v1, v2);
}

// 0x40fb50: debris thrown in the air.
inline int World::actionDebris(Sprite& s, int a, int v1, int v2) {
    const VidInfo& v = vidOf(s);
    switch (a) {
        case act::go: return 0;
        case act::path_block:
        case act::path_limit: changeAction(s, act::death, -1); return 0;
        case act::nextcommand:
            if (s.action == act::death) return 0;
            if (s.z <= 0) { s.goZ = 0; s.z = 0; s.vz = 0; changeAction(s, act::death, -1); return 0; }
            changeAction(s, act::go, -1);
            return 0;
        case act::init:
            s.state |= st::Accelerate;
            s.hp = v.maxHP;
            s.vz = v.speedY;
            changeAction(s, act::go, rand() & 0xff);
            s.goZ = 0;
            return 0;
        default: return actionMaterial(s, a, v1, v2);
    }
}

// 0x40fc70: shells ejected by the guns.
inline int World::actionShell(Sprite& s, int a, int v1, int v2) {
    const VidInfo& v = vidOf(s);
    switch (a) {
        case act::path_block:
        case act::path_limit: changeAction(s, act::death, -1); return 0;
        case act::nextcommand:
            if (s.action == act::death) return 0;
            if (s.z <= 0) { s.goZ = 0; s.z = 0; s.vz = 0; changeAction(s, act::death, -1); return 0; }
            changeAction(s, act::go, -1);
            return 0;
        case act::init:
            s.goZ = 0;
            s.state |= st::Accelerate;
            s.hp = v.maxHP;
            s.vz = v.speedY;
            changeAction(s, act::go, -1);
            return 0;
        case act::child: {
            const int r = rand() % 17;
            setDirection(s, static_cast<std::uint8_t>(s.direction - r - 0x78));
            return 0;
        }
        default: return actionMaterial(s, a, v1, v2);
    }
}

// 0x40fd70: aircraft falling down.
inline int World::actionDownedAvia(Sprite& s, int a, int v1, int v2) {
    const VidInfo& v = vidOf(s);
    switch (a) {
        case act::go: return 0;
        case act::path_block:
        case act::path_limit: stop(s); changeAction(s, act::death, -1); return 0;
        case act::nextcommand:
            if (s.action == act::death) return 0;
            if (s.z <= 0) { s.goZ = 0; s.z = 0; s.vz = 0; changeAction(s, act::death, -1); return 0; }
            changeAction(s, act::r_rotate, fx::rotateStep(v.dirs, s.direction, true));
            return 0;
        case act::init:
            s.state |= st::Accelerate;
            s.goZ = 0;
            s.hp = v.maxHP;
            s.vz = static_cast<std::int16_t>(-v.speedY);
            changeAction(s, act::go, -1);
            return 0;
        default: return actionMaterial(s, a, v1, v2);
    }
}

// 0x410d00: drifting objects (class "N/A").
inline int World::actionNA(Sprite& s, int a, int v1, int v2) {
    switch (a) {
        case act::go: return 0;
        case act::path_limit:
            s.x = static_cast<std::int16_t>(s.x + v1);
            s.y = static_cast<std::int16_t>(s.y + v2);
            changeAction(s, act::death, -1);
            return 0;
        case act::path_block:
            s.x = static_cast<std::int16_t>(s.x + v1);
            s.y = static_cast<std::int16_t>(s.y + v2);
            return 0;
        case act::nextcommand:
            if (s.action != act::death) changeAction(s, act::go, -1);
            return 0;
        case act::init:
            s.state |= st::Accelerate;
            s.speed = 10;
            s.hp = vidOf(s).maxHP;
            s.z = s.goZ = 0x55;
            setDirection(s, 0x40);
            return 0;
        default: return actionMaterial(s, a, v1, v2);
    }
}

// 0x40dd30: screen images (menus): fixed to the screen.
inline int World::actionImage(Sprite& s, int a, int v1, int v2) {
    switch (a) {
        case 0: case 1: case 2: case 3: case 4: case 5: case act::save: return 0;
        case act::init:
            for (Sprite* c = &s; c; c = c->child) c->state |= st::Screen;
            return 0;
        case act::nextcommand:
            if (s.action == act::death) return 0;
            if (s.action == act::l_rotate) { changeAction(s, act::go, -1); s.state |= st::SoundDone; }
            else if (s.action == act::r_rotate) { changeAction(s, act::start_move, -1); s.state |= st::SoundDone; }
            if (s.action == act::birth) changeAction(s, act::stand, -1);
            return 0;
        case act::saveframe: return 0;
        default: return actionBase(s, a, v1, v2);
    }
}

// 0x40fec0: rising effects (smoke): play the death animation while going up.
inline int World::actionEffect1(Sprite& s, int a, int v1, int v2) {
    switch (a) {
        case act::nextcommand:
            if (s.action != act::death) changeAction(s, act::death, -1);
            return 0;
        case act::init:
            s.hp = vidOf(s).maxHP;
            changeAction(s, act::death, -1);
            [[fallthrough]];
        case act::child:
            s.goZ = 0x1000;
            s.vz = vidOf(s).speedY;
            return 0;
        default: return actionMaterial(s, a, v1, v2);
    }
}

// 0x412a30: effects attached to the parent's sub-object chain.
inline int World::actionEffect2(Sprite& s, int a, int v1, int v2) {
    if (a != act::child) return actionBase(s, a, v1, v2);
    Sprite* p = byId(v1);
    if (!p) return 0;
    Sprite* last = p;
    while (last->child) last = last->child;
    last->dx = static_cast<std::int8_t>(s.x - last->x);
    last->dy = static_cast<std::int8_t>(s.y - last->y);
    last->dz = static_cast<std::int8_t>(s.z - last->z);
    last->child = &s;
    s.state |= st::Sub;
    if (!(vidOf(*last).flags & vf::InvisibleSubobjects)) listRemove(&s);
    if (p->action == act::fight) {
        s.attackX = s.goX; s.attackY = s.goY; s.attackZ = s.goZ;
        s.action = act::fight;
        createChildren(s);
        changeAction(s, act::death, -1);
    }
    return 0;
}

// 0x411560: mines (fixed or mobile).
inline int World::actionMine(Sprite& s, int a, int v1, int v2) {
    switch (a) {
        case act::nextcommand: {
            if (s.z < 0) { s.z = 0; s.goZ = 0; if (s.vz < 0) s.vz = 0; }
            if (s.action == act::death) return 0;
            if ((s.state & 3) == 3) stop(s);
            Sprite* t = byId(s.target);
            if (t && distance(s, *t) > vidOf(s).visibilityRadius) t = nullptr;
            s.target = idOf(t);
            if (!t && (s.behave & 1)) { t = findTarget(s); s.target = idOf(t); }
            if (t) {
                if (distance(s, *t) < static_cast<std::int16_t>(weaponOf(s).weaponRange)) {
                    changeAction(s, act::death, -1);
                    return actionVehicle(s, a, v1, v2);
                }
                if (!(s.behave & 4)) approach(s, t->x, t->y);
            }
            return actionVehicle(s, a, v1, v2);
        }
        case act::path_block: {
            Sprite* b = askPlace(s, s.x + v1, s.y + v2, s.z);
            if (b) {
                if (b->nvid == s.nvid) s.direction = static_cast<std::uint8_t>(s.direction + 0x80);
                else { stop(s); changeAction(s, act::death, -1); }
            } else if (s.z <= 0) {
                stop(s);
                changeAction(s, act::death, -1);
            }
            if (s.z > 0) { s.goZ = 0; s.z = 0; s.vz = 0; }
            return 0;
        }
        case act::path_limit: stop(s); changeAction(s, act::death, -1); return 0;
        case act::child: {
            if (Sprite* p = byId(v1))
                for (auto& pl : m_players)
                    if (Sprite* f = byId(pl.flagman); f && f->child == p) { s.army = f->army; break; }
            return actionVehicle(s, a, v1, v2);
        }
        default: return actionVehicle(s, a, v1, v2);
    }
}

// 0x40ff60: ballistic missiles of the cannons.
inline int World::actionCannonMissile(Sprite& s, int a, int v1, int v2) {
    const VidInfo& v = vidOf(s);
    switch (a) {
        case act::go:
        case act::move: return 0;
        case act::path_block: changeAction(s, act::death, -1); return 0;
        case act::path_limit:
            s.x = static_cast<std::int16_t>(s.x + v1);
            s.y = static_cast<std::int16_t>(s.y + v2);
            if (s.x < -120 || s.y < -100 || s.x > m_mapW + 120 || s.y > m_mapH + 100) changeAction(s, act::death, -1);
            return 0;
        case act::attack: {
            Sprite* t = byId(v1);
            if (!t || t == &s) return 0;
            setDestination(s, t->x, t->y, t->z);
            changeAction(s, act::go, -1);
            return 0;
        }
        case act::nextcommand:
            if ((s.state & 3) == 3 && std::abs(s.goZ - s.z) < v.sizeZ + 20) { stop(s); changeAction(s, act::death, -1); }
            if (s.action == act::death) return 0;
            if (s.state & st::Move) {
                std::uint8_t d = getDirection(s.x, s.y, s.goX, s.goY).dir();
                if (s.speed < 0) d = static_cast<std::uint8_t>(d + 0x80);
                if (!turnTo(s, d)) s.frame = static_cast<std::int16_t>(s.lastFrame + 1);
                else if (s.action != act::go) changeAction(s, act::go, -1);
            }
            if (s.speed == 0 || s.timer == 0) changeAction(s, act::death, -1);
            return 0;
        case act::init:
            s.timer = 3000;
            s.hp = v.maxHP;
            return 0;
        case act::deletetarget: removeAttackCommands(s, v1); return 0;
        default: return actionMaterial(s, a, v1, v2);
    }
}

// 0x4101e0: homing missiles.
inline int World::actionAviaMissile(Sprite& s, int a, int v1, int v2) {
    const VidInfo& v = vidOf(s);
    switch (a) {
        case act::go: return 0;
        case act::deletetarget:
            if (s.target == v1) s.target = 0;
            removeAttackCommands(s, v1);
            return 0;
        case act::path_block: changeAction(s, act::death, -1); return 0;
        case act::path_limit:
            s.x = static_cast<std::int16_t>(s.x + v1);
            s.y = static_cast<std::int16_t>(s.y + v2);
            if (s.x < -120 || s.y < -100 || s.x > m_mapW + 120 || s.y > m_mapH + 100) changeAction(s, act::death, -1);
            return 0;
        case act::nextcommand: {
            if (s.action == act::death) return 0;
            if (s.speed == 0 || s.timer == 0) { changeAction(s, act::death, -1); return 0; }
            if (Sprite* t = byId(s.target)) {
                s.state &= ~(st::ArrivedX | st::ArrivedY);
                s.goX = t->x; s.goY = t->y; s.goZ = t->z;
                if (std::abs(t->x - s.x) < m_scaleX && std::abs(t->y - s.y) < m_scaleY &&
                    std::abs(t->z - s.z) < vidOf(*t).sizeZ + 20) {
                    changeAction(s, act::death, -1);
                    return 0;
                }
            } else {
                s.target = 0;
                if ((s.state & 3) == 3) stop(s);
            }
            if (!(s.state & st::Move)) return 0;
            std::uint8_t d = getDirection(s.x, s.y, s.goX, s.goY).dir();
            if (s.speed < 0) d = static_cast<std::uint8_t>(d + 0x80);
            if (m_lastDist) {
                int vz = static_cast<std::int16_t>((s.goZ - s.z) * s.speed / m_lastDist);
                if (vz > v.speedY) vz = v.speedY;
                if (vz < -v.speedY) vz = -v.speedY;
                s.vz = static_cast<std::int16_t>(vz);
            }
            if (!turnTo(s, d)) { s.frame = static_cast<std::int16_t>(s.lastFrame + 1); return 0; }
            if (s.action != act::go) changeAction(s, act::go, -1);
            return 0;
        }
        case act::init:
            s.timer = 8000;
            s.target = 0;
            s.hp = v.maxHP;
            return 0;
        case act::child: {
            Sprite* p = byId(v1);
            if (!p) return 0;
            if (!(vidOf(*p).category & (cat::Vehicle | cat::Avia))) {
                const auto ox = s.x, oy = s.y, oz = s.z;
                s.x = p->goX; s.y = p->goY; s.z = p->goZ;
                s.target = idOf(findTarget(s));
                s.x = ox; s.y = oy; s.z = oz;
            } else {
                s.target = p->target;
            }
            if (Sprite* t = byId(s.target)) setDestination(s, t->x, t->y, t->z);
            return 0;
        }
        default: return actionMaterial(s, a, v1, v2);
    }
}

// 0x4117e0: bonuses picked up by the units in their radius.
inline int World::actionBonus(Sprite& s, int a, int v1, int v2) {
    switch (a) {
        case act::stand:
        case act::load: return 0;
        case act::path_block: stop(s); return 0;
        case act::nextcommand: {
            if (s.action == act::load) changeAction(s, act::death, -1);
            if (s.action == act::death) return 0;
            if ((s.state & 3) == 3) stop(s);
            if (s.state & st::Move) {
                std::uint8_t d = getDirection(s.x, s.y, s.goX, s.goY).dir();
                if (s.speed < 0) d = static_cast<std::uint8_t>(d + 0x80);
                if (!turnTo(s, d)) s.frame = static_cast<std::int16_t>(s.lastFrame + 1);
                else if (s.action != act::go) changeAction(s, act::go, -1);
            }
            Sprite* t = findTarget(s);
            if (!t) return 0;
            const VidInfo& tv = vidOf(*t);
            if (tv.cls == cls::Mine) return 0;
            const bool kass = tv.cls == cls::Kassandra;
            if (s.nvid == 0x72) {   // medic kit
                if (t->hp >= tv.maxHP) {
                    if (kass) return 0;
                    action(*t, act::additem, 0x72, 0);
                } else if (t->hp <= tv.maxHP - 15) {
                    action(*t, act::damage, -15, 0);
                } else {
                    action(*t, act::damage, t->hp - tv.maxHP, 0);
                }
            } else if (s.nvid == 0xe6 && kass) {
                if (t->ammo[1] >= 200) return 0;
                action(*t, act::additem, 0xe6, 0);
            } else if (s.nvid == 0xe7 && kass) {
                if (t->ammo[2] >= 80) return 0;
                action(*t, act::additem, 0xe7, 0);
            } else if (s.nvid == 0xe8 && kass) {
                if (t->ammo[3] >= 40) return 0;
                action(*t, act::additem, 0xe8, 0);
            } else if (s.nvid == 0xe9 && kass) {
                if (t->ammo[4] >= 500) return 0;
                action(*t, act::additem, 0xe9, 0);
            } else {
                action(*t, act::additem, s.nvid, 0);
            }
            if (!s.alive) return 0;
            changeAction(s, act::load, -1);
            if (!vidOf(s).animLen[act::load]) s.frame = s.lastFrame;
            return 0;
        }
        default: return actionMaterial(s, a, v1, v2);
    }
}

// 0x40dea0: text sprites. The text itself is owned by the session (PrintText).
inline int World::actionFont(Sprite& s, int a, int v1, int v2) {
    switch (a) {
        case act::settext:
        case act::gettext:
        case act::setfile:
        case act::nextcommand:
        case act::init:
        case act::additem: return 0;
        default: return actionBase(s, a, v1, v2);
    }
}

// 0x411a60: the player's vehicle. Its act_nextcommand reads the player's input record.
inline int World::actionKassandra(Sprite& s, int a, int v1, int v2) {
    const VidInfo& v = vidOf(s);
    switch (a) {
        case act::move:
            if (!v.speedX) return 0;
            s.state &= ~st::Attack;
            s.target = 0;
            approach(s, v1, v2);
            return 0;
        case act::jump:
            if (!s.ammo[4] || askPlace(s, v1, v2, 0)) return 0;
            s.ammo[4]--;
            return actionVehicle(s, a, v1, v2);
        case act::coor_attack: {
            s.state &= ~st::Invisible;
            Sprite* t = nullptr;
            if (s.army < MaxPlayers) {
                t = byId(m_players[s.army].underCursor);
                if (t && !(vidOf(*t).category & static_cast<std::uint8_t>(weaponOf(s).targetCategory))) t = nullptr;
            }
            if (!s.child) return 0;
            const int h = s.child->nvid;
            int wi;
            if (h < 0x5d) wi = h - 0x58;
            else if (h < 0xcf) wi = h - 0xca;
            else if (h == 0xcf) wi = 4;
            else wi = h - 0xcf;
            const bool validWi = wi >= 0 && wi < 8;
            const int perShot = vid(h).childCount[act::fight];
            if (validWi && s.ammo[static_cast<std::size_t>(wi)] < perShot) return 0;
            int fired;
            if (t) {
                const auto tsz = vidOf(*t).sizeZ;
                fired = fire(s, v1, t->y, t->z - (tsz >> 2) + tsz);   // x from the mouse (sic)
            } else if (weaponOf(s).flags & 8) {
                fired = fire(s, v1, v2 + 80, 80);
            } else {
                fired = fire(s, v1, v2, 19);
            }
            if (fired && validWi && s.child) {
                if (s.child->nvid == 0xd1) s.ammo[static_cast<std::size_t>(wi)] -= 4;
                else s.ammo[static_cast<std::size_t>(wi)] = static_cast<std::int16_t>(s.ammo[static_cast<std::size_t>(wi)] - vid(s.child->nvid).childCount[act::fight]);
            }
            return fired;
        }
        case act::addammo:
            if (v2 >= 8 || v2 < 0) return 0;
            s.ammo[static_cast<std::size_t>(v2)] = static_cast<std::int16_t>(s.ammo[static_cast<std::size_t>(v2)] + v1);
            if (s.ammo[static_cast<std::size_t>(v2)] < 0) s.ammo[static_cast<std::size_t>(v2)] = 0;
            return 0;
        case act::getammo:
            if (v1 >= 8 || v1 < 0) return 0;
            return s.ammo[static_cast<std::size_t>(v1)];
        case act::changeweapon: {
            if (!v.linkedObjectVid || !s.child || s.child->commands) return 0;
            if (v1 < 0 || v1 >= static_cast<int>(m_vids.size())) return 0;
            if (s.army < MaxPlayers) m_players[s.army].weapon = vid(v1).weapon;
            s.child->state |= s.state & st::GammaMask;
            pushCommandTop(*s.child, act::changevid, v1, act::unload);
            pushCommandTop(*s.child, act::load, 0, 0);
            return 1;
        }
        case act::nextcommand: {
            if (s.action == act::death) return 0;
            if ((s.state & 3) == 3) stop(s);
            const InputRecord in = (s.army < MaxPlayers && m_netGame) ? m_players[s.army].input : m_localInput;
            const auto bits = in.bits;
            if (s.wpX >= 0 && s.speed >= 0) {
                if (std::abs(s.wpX - s.x) <= m_scaleX && std::abs(s.wpY - s.y) <= m_scaleY) clearWaypoint(s);
                else if (!(s.state & st::Move)) setDestination(s, s.wpX, s.wpY, s.z);
            } else if (!(bits & 0xc0)) {
                changeAction(s, act::stand, -1);
            }
            if (s.state & st::Move) {
                std::uint8_t d = getDirection(s.x, s.y, s.goX, s.goY).dir();
                if (s.speed < 0) d = static_cast<std::uint8_t>(d + 0x80);
                if (!turnTo(s, d)) s.frame = static_cast<std::int16_t>(s.lastFrame + 1);
                else if (s.action != act::go) changeAction(s, act::go, -1);
            }
            if (bits & 0x40) s.state |= st::Accelerate;
            else if (!(s.state & st::Move)) s.state &= ~st::Accelerate;
            if (bits & 0x80) {
                s.speed = static_cast<std::int16_t>(-(vidOf(s).speedX >> 1));
                s.state &= ~st::Accelerate;
            } else if (s.speed < 0 && !(s.state & st::Move)) {
                s.speed = 0;
                s.state &= ~st::Accelerate;
            }
            if (bits & 0xc0) {
                s.state &= ~(st::ArrivedX | st::ArrivedY | st::Move);
                s.wpX = s.wpY = -1;
                if (s.speed) { if (s.action != act::go) changeAction(s, act::go, -1); }
                else changeAction(s, act::start_move, -1);
            }
            const int dirs = vidOf(s).dirs;
            if (bits & 4) {
                if (s.speed > 0) changeAction(s, act::l_rotate, fx::rotateStep(dirs, s.direction, false));
                else if (s.speed < 0) changeAction(s, act::l_rotate, fx::rotateStep(dirs, s.direction, true));
            }
            if (bits & 8) {
                if (s.speed > 0) changeAction(s, act::r_rotate, fx::rotateStep(dirs, s.direction, true));
                else if (s.speed < 0) changeAction(s, act::r_rotate, fx::rotateStep(dirs, s.direction, false));
            }
            return 0;
        }
        case act::additem: addItem(s, v1); return 0;
        case act::damage: return actionMaterial(s, a, v1, v2);
        case act::destroy:
            vidOf(s).weapon = vid(88).weapon;   // back to the default head's weapon (0x6ac9)
            return actionVehicle(s, a, v1, v2);
        case act::path_block: {
            if (s.vz) {
                if (v1 == 0 && v2 == 0) {
                    if (s.state & st::Jump) {
                        changeAction(s, act::fight, -1);
                        s.state &= ~st::Jump;
                        s.speed = 0;
                        s.vz = 0;
                    } else {
                        s.vz = 0;
                    }
                } else {
                    changeAction(s, act::death, -1);
                }
                return 0;
            }
            if (s.wpX < 0) return 0;
            std::uint8_t d = getDirection(s.x, s.y, s.goX, s.goY).dir();
            if (!turnTo(s, d)) { s.frame = static_cast<std::int16_t>(s.lastFrame + 1); return 0; }
            if (s.goX == s.wpX && s.goY == s.wpY) {
                for (int k = 16; k <= 64; k += 16) {
                    for (int side = 0; side < 2; ++side) {
                        const std::uint8_t dd = static_cast<std::uint8_t>(side ? d - k : d + k);
                        const auto px = static_cast<std::int16_t>(s.x + (fx::Sin[dd] >> 6));
                        const auto py = static_cast<std::int16_t>(s.y + (fx::NegCos[dd] >> 6));
                        if (!askPlace(s, px, py, s.z)) { setDestination(s, px, py, s.z); return 0; }
                    }
                }
                return 0;
            }
            if (s.action != act::go) changeAction(s, act::go, -1);
            const int r = rand();
            const std::uint8_t d2 = static_cast<std::uint8_t>((r & 1) ? s.direction + 0x40 : s.direction - 0x40);
            setDestination(s, s.x + (fx::Sin[d2] >> 5), s.y + (fx::NegCos[d2] >> 5), s.z);
            s.speed = static_cast<std::int16_t>((-vidOf(s).speedX) >> 1);
            s.state &= ~st::Accelerate;
            return 0;
        }
        case act::init:
            s.ammo.fill(0);
            return actionVehicle(s, a, v1, v2);
        case act::death:
            for (int i = 1; i < 4; ++i) {
                if (!s.ammo[static_cast<std::size_t>(i)]) continue;
                if (!(rand() & 1)) continue;
                const int dir = rand() & 0xff;
                if (Sprite* c = createUnit(0xe5 + i, s.x, s.y, s.z + 10, 0, dir, nullptr)) {
                    c->state |= st::Accelerate;
                    c->goZ = 0;
                    c->vz = vidOf(*c).speedY;
                }
            }
            return actionVehicle(s, a, v1, v2);
        default: return actionVehicle(s, a, v1, v2);
    }
}

} // namespace gromada::game
