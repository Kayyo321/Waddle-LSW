/**
 * @file host_wayland_windows_stress.h
 * @brief Randomized long sequences (TODO #11h). Each segment starts a fresh decorated session and runs random
 * announce, update, retire, configure, popup_done, decoration, press and failure-injection operations, with a
 * fixed-seed generator so every run is reproducible. Invariants are checked after every operation:
 *  - the records agree with the window table (each realized window has one record with its objects);
 *  - no destroy left a live dependent behind (checked by the double: decoration before toplevel, children
 *    before parents, wl_surface after xdg_surface);
 *  - no success callback names a window the table does not hold realized;
 *  - no proxy was destroyed twice.
 * Each segment ends with a reset whose creates and destroys must balance exactly, so nothing leaks.
 */
#ifndef HostWaylandWindowsStressH
#define HostWaylandWindowsStressH

static uint64_t stress_state;

static uint64_t stress_next(void) {
    /* xorshift64*: small, deterministic and good enough for interleavings. */
    stress_state ^= stress_state >> 12;
    stress_state ^= stress_state << 25;
    stress_state ^= stress_state >> 27;
    return stress_state * 2685821657736338717ull;
}

static unsigned stress_below(unsigned bound) {
    return bound == 0 ? 0u : (unsigned)(stress_next() % bound);
}

/** @brief Records and the table must agree, and the double's invariant counters must be clean. */
static int stress_invariants(void) {
    unsigned records = 0;
    for (unsigned index = 0; index < HostWindowMapMaxRealized; ++index) {
        const host_wayland_record_t *rec = &session_ctx.records[index];
        if (rec->generation == 0) continue;
        ++records;
        host_window_ref_t id;
        id.window = rec->window;
        id.generation = rec->generation;
        Check(host_window_map_is_current(&session_ctx.map, id));
        Check(rec->surface != NULL && rec->xdg_surface != NULL);
        if (role_is_popup(rec->role)) Check(rec->popup != NULL && rec->toplevel == NULL);
        else Check(rec->toplevel != NULL && rec->popup == NULL);
    }
    Check(records == host_wayland_windows_window_count(&session_ctx));
    Check(fake_order_violations == 0 && fake_double_destroys == 0);
    Check(captured.stale_callbacks == 0);
    return 0;
}

/** @brief Random live-or-dead proxy of `iface`, or FakeNone. Dead proxies are kept on purpose: late events. */
static unsigned stress_pick(const char *iface) {
    unsigned matches = 0;
    for (unsigned index = 0; index < fake_proxy_count; ++index) {
        if (fake_proxies[index].iface && !strcmp(fake_proxies[index].iface, iface)) ++matches;
    }
    if (matches == 0) return FakeNone;
    unsigned choice = stress_below(matches);
    for (unsigned index = 0; index < fake_proxy_count; ++index) {
        if (fake_proxies[index].iface && !strcmp(fake_proxies[index].iface, iface)) {
            if (choice == 0) return index;
            --choice;
        }
    }
    return FakeNone;
}

static const char *const stress_fail_names[] = {"wl_surface", "xdg_surface", "xdg_toplevel", "xdg_positioner", "xdg_popup", "zxdg_toplevel_decoration_v1"};

/** @brief One random operation of the interleaving. Returns non-zero on a failed invariant. */
static int stress_step(uint64_t *now_ms, unsigned *press_serial) {
    unsigned choice = stress_below(100);
    *now_ms += 1 + stress_below(120);
    if (choice < 25) {
        /* Announce: any role, parent sometimes a realized window, sometimes garbage; failures sometimes injected. */
        uint64_t window = 1 + stress_below(10);
        uint64_t generation = 1 + stress_below(2);
        announce_spec_t spec = toplevel_spec(window, generation, "a-stress");
        spec.role = 1 + stress_below(6);
        if (spec.role == RoleUac) spec.role = RoleToplevel;
        if (stress_below(3) == 0) spec.title = "Stress";
        if (stress_below(4) == 0) {
            spec.has_min_size = true;
            spec.min_width = 100 + stress_below(900);
            spec.min_height = 100 + stress_below(900);
        }
        bool wants_parent = spec.role == RoleDialog || role_is_popup(spec.role);
        if (wants_parent && (role_is_popup(spec.role) || stress_below(2))) {
            const host_wayland_record_t *parent = NULL;
            for (unsigned index = 0; index < HostWindowMapMaxRealized && !parent; ++index) {
                if (session_ctx.records[index].generation && stress_below(3) == 0) parent = &session_ctx.records[index];
            }
            spec.has_parent = true;
            spec.parent = parent ? parent->window : 1 + stress_below(10);
            spec.parent_generation = parent ? parent->generation : 1;
        }
        if (spec.role == RoleMenu && stress_below(2)) {
            spec.has_serial = true;
            spec.serial = *press_serial + stress_below(3);
        }
        const char *injected = NULL;
        if (stress_below(8) == 0) {
            injected = stress_fail_names[stress_below(6)];
            if (stress_below(2)) fake_fail_create = injected;
            else fake_fail_listener = injected;
        }
        (void)announce_spec(&session_ctx, &spec);
        fake_fail_create = NULL;
        fake_fail_listener = NULL;
    } else if (choice < 40) {
        update_spec_t update;
        memset(&update, 0, sizeof update);
        update.window = 1 + stress_below(10);
        update.generation = 1 + stress_below(2);
        if (stress_below(2)) update.title = "Renamed";
        if (stress_below(3) == 0) {
            update.has_parent = true;
            update.parent = 1 + stress_below(10);
            update.parent_generation = 1 + stress_below(2);
        }
        if (stress_below(3) == 0) {
            update.has_min_size = true;
            update.min_width = 100 + stress_below(900);
            update.min_height = 100 + stress_below(900);
        }
        (void)update_frame(&update);
    } else if (choice < 52) {
        (void)retire_frame(1 + stress_below(10), 1 + stress_below(2));
    } else if (choice < 62) {
        unsigned surface = stress_pick("xdg_surface");
        if (surface != FakeNone) deliver_surface_configure(surface, stress_below(1000));
    } else if (choice < 68) {
        unsigned toplevel = stress_pick("xdg_toplevel");
        if (toplevel != FakeNone) {
            if (stress_below(2)) {
                const uint32_t states[] = {XDG_TOPLEVEL_STATE_MAXIMIZED, XDG_TOPLEVEL_STATE_ACTIVATED};
                deliver_toplevel_configure(toplevel, (int32_t)stress_below(2000), (int32_t)stress_below(2000), states, stress_below(3));
            } else {
                deliver_toplevel_close(toplevel);
            }
        }
    } else if (choice < 76) {
        unsigned popup = stress_pick("xdg_popup");
        if (popup != FakeNone) {
            if (stress_below(3) == 0) deliver_popup_done(popup);
            else deliver_popup_configure(popup, (int32_t)stress_below(400) - 200, (int32_t)stress_below(400) - 200, 40, 20);
        }
    } else if (choice < 82) {
        unsigned decoration = stress_pick("zxdg_toplevel_decoration_v1");
        if (decoration != FakeNone) {
            const uint32_t modes[] = {DecorationModeClient, DecorationModeServer, 7u};
            deliver_decoration_configure(decoration, modes[stress_below(3)]);
        }
    } else if (choice < 90) {
        unsigned seat = fake_nth("wl_seat", 0);
        if (seat != FakeNone) {
            *press_serial += 1;
            (void)record_press_at(&session_ctx, *press_serial, 900 + *press_serial, (struct wl_seat *)fake_object(seat), 1 + stress_below(10), 1 + stress_below(2), *now_ms);
        }
    } else {
        host_wayland_windows_release_press(&session_ctx, *press_serial - stress_below(3));
    }
    return stress_invariants();
}

/** @brief One segment: a fresh session, `steps` operations, then a reset whose creates and destroys balance. */
static int stress_segment(unsigned steps) {
    Check(begin_decorated_session() == 0);
    capture_reset();
    uint64_t now_ms = 1000;
    unsigned press_serial = 1;
    /* The seat is borrowed like the other fixtures: it is created here and never destroyed by the module. */
    fake_new("wl_seat");
    fixture_creates = fake_creates;
    for (unsigned step = 0; step < steps; ++step) {
        if (stress_step(&now_ms, &press_serial) != 0) return 1;
    }
    unsigned creates = fake_creates, destroys = fake_destroys;
    Check(host_wayland_windows_reset(&session_ctx) == HostWaylandOk);
    Check(fake_destroys - destroys == creates - destroys - fixture_creates);
    Check(fake_creates - fixture_creates == fake_destroys && fake_double_destroys == 0 && fake_order_violations == 0);
    Check(captured.stale_callbacks == 0);
    return 0;
}

static int check_stress(void) {
    /* Fixed seeds: each run replays the same interleavings, so a failure is reproducible from the seed alone. */
    const uint64_t seeds[] = {0x1234567890ABCDEFull, 0x0F0F1E1E2D2D3C3Cull, 0xDEADBEEFCAFEF00Dull, 42u};
    for (unsigned index = 0; index < sizeof seeds / sizeof seeds[0]; ++index) {
        stress_state = seeds[index];
        for (unsigned segment = 0; segment < 40; ++segment) {
            if (stress_segment(60) != 0) {
                fprintf(stderr, "stress seed index %u segment %u failed\n", index, segment);
                return 1;
            }
        }
    }
    return 0;
}

#endif
