/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2013 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <sys/capability.h>

#include "strv.h"
#include "set.h"
#include "bus-internal.h"
#include "bus-message.h"
#include "bus-type.h"
#include "bus-signature.h"
#include "bus-introspect.h"
#include "bus-objects.h"
#include "bus-util.h"

static int node_vtable_get_userdata(
                sd_bus *bus,
                const char *path,
                struct node_vtable *c,
                void **userdata,
                sd_bus_error *error) {

        void *u;
        int r;

        assert(bus);
        assert(path);
        assert(c);

        u = c->userdata;
        if (c->find) {
                r = c->find(bus, path, c->interface, u, &u, error);
                if (r < 0)
                        return r;
                if (sd_bus_error_is_set(error))
                        return sd_bus_error_get_errno(error);
                if (r == 0)
                        return r;
        }

        if (userdata)
                *userdata = u;

        return 1;
}

static void *vtable_property_convert_userdata(const sd_bus_vtable *p, void *u) {
        assert(p);

        return (uint8_t*) u + p->x.property.offset;
}

static int vtable_property_get_userdata(
                sd_bus *bus,
                const char *path,
                struct vtable_member *p,
                void **userdata,
                sd_bus_error *error) {

        void *u;
        int r;

        assert(bus);
        assert(path);
        assert(p);
        assert(userdata);

        r = node_vtable_get_userdata(bus, path, p->parent, &u, error);
        if (r <= 0)
                return r;
        if (bus->nodes_modified)
                return 0;

        *userdata = vtable_property_convert_userdata(p->vtable, u);
        return 1;
}

static int add_enumerated_to_set(
                sd_bus *bus,
                const char *prefix,
                struct node_enumerator *first,
                Set *s,
                sd_bus_error *error) {

        struct node_enumerator *c;
        int r;

        assert(bus);
        assert(prefix);
        assert(s);

        LIST_FOREACH(enumerators, c, first) {
                char **children = NULL, **k;

                if (bus->nodes_modified)
                        return 0;

                r = c->callback(bus, prefix, c->userdata, &children, error);
                if (r < 0)
                        return r;
                if (sd_bus_error_is_set(error))
                        return sd_bus_error_get_errno(error);

                STRV_FOREACH(k, children) {
                        if (r < 0) {
                                free(*k);
                                continue;
                        }

                        if (!object_path_is_valid(*k)){
                                free(*k);
                                r = -EINVAL;
                                continue;
                        }

                        if (!object_path_startswith(*k, prefix)) {
                                free(*k);
                                continue;
                        }

                        r = set_consume(s, *k);
                        if (r == -EEXIST)
                                r = 0;
                }

                free(children);
                if (r < 0)
                        return r;
        }

        return 0;
}

static int add_subtree_to_set(
                sd_bus *bus,
                const char *prefix,
                struct node *n,
                Set *s,
                sd_bus_error *error) {

        struct node *i;
        int r;

        assert(bus);
        assert(prefix);
        assert(n);
        assert(s);

        r = add_enumerated_to_set(bus, prefix, n->enumerators, s, error);
        if (r < 0)
                return r;
        if (bus->nodes_modified)
                return 0;

        LIST_FOREACH(siblings, i, n->child) {
                char *t;

                if (!object_path_startswith(i->path, prefix))
                        continue;

                t = strdup(i->path);
                if (!t)
                        return -ENOMEM;

                r = set_consume(s, t);
                if (r < 0 && r != -EEXIST)
                        return r;

                r = add_subtree_to_set(bus, prefix, i, s, error);
                if (r < 0)
                        return r;
                if (bus->nodes_modified)
                        return 0;
        }

        return 0;
}

static int get_child_nodes(
                sd_bus *bus,
                const char *prefix,
                struct node *n,
                Set **_s,
                sd_bus_error *error) {

        Set *s = NULL;
        int r;

        assert(bus);
        assert(prefix);
        assert(n);
        assert(_s);

        s = set_new(string_hash_func, string_compare_func);
        if (!s)
                return -ENOMEM;

        r = add_subtree_to_set(bus, prefix, n, s, error);
        if (r < 0) {
                set_free_free(s);
                return r;
        }

        *_s = s;
        return 0;
}

static int node_callbacks_run(
                sd_bus *bus,
                sd_bus_message *m,
                struct node_callback *first,
                bool require_fallback,
                bool *found_object) {

        struct node_callback *c;
        int r;

        assert(bus);
        assert(m);
        assert(found_object);

        LIST_FOREACH(callbacks, c, first) {
                _cleanup_bus_error_free_ sd_bus_error error_buffer = SD_BUS_ERROR_NULL;

                if (bus->nodes_modified)
                        return 0;

                if (require_fallback && !c->is_fallback)
                        continue;

                *found_object = true;

                if (c->last_iteration == bus->iteration_counter)
                        continue;

                c->last_iteration = bus->iteration_counter;

                r = sd_bus_message_rewind(m, true);
                if (r < 0)
                        return r;

                r = c->callback(bus, m, c->userdata, &error_buffer);
                r = bus_maybe_reply_error(m, r, &error_buffer);
                if (r != 0)
                        return r;
        }

        return 0;
}

#define CAPABILITY_SHIFT(x) (((x) >> __builtin_ctzll(_SD_BUS_VTABLE_CAPABILITY_MASK)) & 0xFFFF)

static int check_access(sd_bus *bus, sd_bus_message *m, struct vtable_member *c, sd_bus_error *error) {
        _cleanup_bus_creds_unref_ sd_bus_creds *creds = NULL;
        uint64_t cap;
        uid_t uid;
        int r;

        assert(bus);
        assert(m);
        assert(c);

        /* If the entire bus is trusted let's grant access */
        if (bus->trusted)
                return 0;

        /* If the member is marked UNPRIVILEGED let's grant access */
        if (c->vtable->flags & SD_BUS_VTABLE_UNPRIVILEGED)
                return 0;

        /* If we are not connected to kdbus we cannot retrieve the
         * effective capability set without race. Since we need this
         * for a security decision we cannot use racy data, hence
         * don't request it. */
        if (bus->is_kernel)
                r = sd_bus_query_sender_creds(m, SD_BUS_CREDS_UID|SD_BUS_CREDS_EFFECTIVE_CAPS, &creds);
        else
                r = sd_bus_query_sender_creds(m, SD_BUS_CREDS_UID, &creds);
        if (r < 0)
                return r;

        /* Check have the caller has the requested capability
         * set. Note that the flags value contains the capability
         * number plus one, which we need to subtract here. We do this
         * so that we have 0 as special value for "default
         * capability". */
        cap = CAPABILITY_SHIFT(c->vtable->flags);
        if (cap == 0)
                cap = CAPABILITY_SHIFT(c->parent->vtable[0].flags);
        if (cap == 0)
                cap = CAP_SYS_ADMIN;
        else
                cap --;

        r = sd_bus_creds_has_effective_cap(creds, cap);
        if (r > 0)
                return 1;

        /* Caller has same UID as us, then let's grant access */
        r = sd_bus_creds_get_uid(creds, &uid);
        if (r >= 0) {
                if (uid == getuid())
                        return 1;
        }

        return sd_bus_error_setf(error, SD_BUS_ERROR_ACCESS_DENIED, "Access to %s.%s() not permitted.", c->interface, c->member);
}

static int method_callbacks_run(
                sd_bus *bus,
                sd_bus_message *m,
                struct vtable_member *c,
                bool require_fallback,
                bool *found_object) {

        _cleanup_bus_error_free_ sd_bus_error error = SD_BUS_ERROR_NULL;
        const char *signature;
        void *u;
        int r;

        assert(bus);
        assert(m);
        assert(c);
        assert(found_object);

        if (require_fallback && !c->parent->is_fallback)
                return 0;

        r = check_access(bus, m, c, &error);
        if (r < 0)
                return bus_maybe_reply_error(m, r, &error);

        r = node_vtable_get_userdata(bus, m->path, c->parent, &u, &error);
        if (r <= 0)
                return bus_maybe_reply_error(m, r, &error);
        if (bus->nodes_modified)
                return 0;

        *found_object = true;

        if (c->last_iteration == bus->iteration_counter)
                return 0;

        c->last_iteration = bus->iteration_counter;

        r = sd_bus_message_rewind(m, true);
        if (r < 0)
                return r;

        signature = sd_bus_message_get_signature(m, true);
        if (!signature)
                return -EINVAL;

        if (!streq(strempty(c->vtable->x.method.signature), signature))
                return sd_bus_reply_method_errorf(
                                m,
                                SD_BUS_ERROR_INVALID_ARGS,
                                "Invalid arguments '%s' to call %s.%s(), expecting '%s'.",
                                signature, c->interface, c->member, strempty(c->vtable->x.method.signature));

        /* Keep track what the signature of the reply to this message
         * should be, so that this can be enforced when sealing the
         * reply. */
        m->enforced_reply_signature = strempty(c->vtable->x.method.result);

        if (c->vtable->x.method.handler) {
                r = c->vtable->x.method.handler(bus, m, u, &error);
                return bus_maybe_reply_error(m, r, &error);
        }

        /* If the method callback is NULL, make this a successful NOP */
        r = sd_bus_reply_method_return(m, NULL);
        if (r < 0)
                return r;

        return 1;
}

static int invoke_property_get(
                sd_bus *bus,
                const sd_bus_vtable *v,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        const void *p;
        int r;

        assert(bus);
        assert(v);
        assert(path);
        assert(interface);
        assert(property);
        assert(reply);

        if (v->x.property.get) {
                r = v->x.property.get(bus, path, interface, property, reply, userdata, error);
                if (r < 0)
                        return r;
                if (sd_bus_error_is_set(error))
                        return sd_bus_error_get_errno(error);
                return r;
        }

        /* Automatic handling if no callback is defined. */

        if (streq(v->x.property.signature, "as"))
                return sd_bus_message_append_strv(reply, *(char***) userdata);

        assert(signature_is_single(v->x.property.signature, false));
        assert(bus_type_is_basic(v->x.property.signature[0]));

        switch (v->x.property.signature[0]) {

        case SD_BUS_TYPE_STRING:
        case SD_BUS_TYPE_SIGNATURE:
                p = strempty(*(char**) userdata);
                break;

        case SD_BUS_TYPE_OBJECT_PATH:
                p = *(char**) userdata;
                assert(p);
                break;

        default:
                p = userdata;
                break;
        }

        return sd_bus_message_append_basic(reply, v->x.property.signature[0], p);
}

static int invoke_property_set(
                sd_bus *bus,
                const sd_bus_vtable *v,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *value,
                void *userdata,
                sd_bus_error *error) {

        int r;

        assert(bus);
        assert(v);
        assert(path);
        assert(interface);
        assert(property);
        assert(value);

        if (v->x.property.set) {
                r = v->x.property.set(bus, path, interface, property, value, userdata, error);
                if (r < 0)
                        return r;
                if (sd_bus_error_is_set(error))
                        return sd_bus_error_get_errno(error);
                return r;
        }

        /*  Automatic handling if no callback is defined. */

        assert(signature_is_single(v->x.property.signature, false));
        assert(bus_type_is_basic(v->x.property.signature[0]));

        switch (v->x.property.signature[0]) {

        case SD_BUS_TYPE_STRING:
        case SD_BUS_TYPE_OBJECT_PATH:
        case SD_BUS_TYPE_SIGNATURE: {
                const char *p;
                char *n;

                r = sd_bus_message_read_basic(value, v->x.property.signature[0], &p);
                if (r < 0)
                        return r;

                n = strdup(p);
                if (!n)
                        return -ENOMEM;

                free(*(char**) userdata);
                *(char**) userdata = n;

                break;
        }

        default:
                r = sd_bus_message_read_basic(value, v->x.property.signature[0], userdata);
                if (r < 0)
                        return r;

                break;
        }

        return 1;
}

static int property_get_set_callbacks_run(
                sd_bus *bus,
                sd_bus_message *m,
                struct vtable_member *c,
                bool require_fallback,
                bool is_get,
                bool *found_object) {

        _cleanup_bus_error_free_ sd_bus_error error = SD_BUS_ERROR_NULL;
        _cleanup_bus_message_unref_ sd_bus_message *reply = NULL;
        void *u;
        int r;

        assert(bus);
        assert(m);
        assert(c);
        assert(found_object);

        if (require_fallback && !c->parent->is_fallback)
                return 0;

        r = vtable_property_get_userdata(bus, m->path, c, &u, &error);
        if (r <= 0)
                return bus_maybe_reply_error(m, r, &error);
        if (bus->nodes_modified)
                return 0;

        *found_object = true;

        r = sd_bus_message_new_method_return(m, &reply);
        if (r < 0)
                return r;

        if (is_get) {
                /* Note that we do not protect against reexecution
                 * here (using the last_iteration check, see below),
                 * should the node tree have changed and we got called
                 * again. We assume that property Get() calls are
                 * ultimately without side-effects or if they aren't
                 * then at least idempotent. */

                r = sd_bus_message_open_container(reply, 'v', c->vtable->x.property.signature);
                if (r < 0)
                        return r;

                /* Note that we do not do an access check here. Read
                 * access to properties is always unrestricted, since
                 * PropertiesChanged signals broadcast contents
                 * anyway. */

                r = invoke_property_get(bus, c->vtable, m->path, c->interface, c->member, reply, u, &error);
                if (r < 0)
                        return bus_maybe_reply_error(m, r, &error);

                if (bus->nodes_modified)
                        return 0;

                r = sd_bus_message_close_container(reply);
                if (r < 0)
                        return r;

        } else {
                if (c->vtable->type != _SD_BUS_VTABLE_WRITABLE_PROPERTY)
                        return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_PROPERTY_READ_ONLY, "Property '%s' is not writable.", c->member);

                /* Avoid that we call the set routine more than once
                 * if the processing of this message got restarted
                 * because the node tree changed. */
                if (c->last_iteration == bus->iteration_counter)
                        return 0;

                c->last_iteration = bus->iteration_counter;

                r = sd_bus_message_enter_container(m, 'v', c->vtable->x.property.signature);
                if (r < 0)
                        return r;

                r = check_access(bus, m, c, &error);
                if (r < 0)
                        return bus_maybe_reply_error(m, r, &error);

                r = invoke_property_set(bus, c->vtable, m->path, c->interface, c->member, m, u, &error);
                if (r < 0)
                        return bus_maybe_reply_error(m, r, &error);

                if (bus->nodes_modified)
                        return 0;

                r = sd_bus_message_exit_container(m);
                if (r < 0)
                        return r;
        }

        r = sd_bus_send(bus, reply, NULL);
        if (r < 0)
                return r;

        return 1;
}

static int vtable_append_all_properties(
                sd_bus *bus,
                sd_bus_message *reply,
                const char *path,
                struct node_vtable *c,
                void *userdata,
                sd_bus_error *error) {

        const sd_bus_vtable *v;
        int r;

        assert(bus);
        assert(reply);
        assert(path);
        assert(c);

        if (c->vtable[0].flags & SD_BUS_VTABLE_HIDDEN)
                return 1;

        for (v = c->vtable+1; v->type != _SD_BUS_VTABLE_END; v++) {
                if (v->type != _SD_BUS_VTABLE_PROPERTY && v->type != _SD_BUS_VTABLE_WRITABLE_PROPERTY)
                        continue;

                if (v->flags & SD_BUS_VTABLE_HIDDEN)
                        continue;

                r = sd_bus_message_open_container(reply, 'e', "sv");
                if (r < 0)
                        return r;

                r = sd_bus_message_append(reply, "s", v->x.property.member);
                if (r < 0)
                        return r;

                r = sd_bus_message_open_container(reply, 'v', v->x.property.signature);
                if (r < 0)
                        return r;

                r = invoke_property_get(bus, v, path, c->interface, v->x.property.member, reply, vtable_property_convert_userdata(v, userdata), error);
                if (r < 0)
                        return r;
                if (bus->nodes_modified)
                        return 0;

                r = sd_bus_message_close_container(reply);
                if (r < 0)
                        return r;

                r = sd_bus_message_close_container(reply);
                if (r < 0)
                        return r;
        }

        return 1;
}

static int property_get_all_callbacks_run(
                sd_bus *bus,
                sd_bus_message *m,
                struct node_vtable *first,
                bool require_fallback,
                const char *iface,
                bool *found_object) {

        _cleanup_bus_message_unref_ sd_bus_message *reply = NULL;
        struct node_vtable *c;
        bool found_interface;
        int r;

        assert(bus);
        assert(m);
        assert(found_object);

        r = sd_bus_message_new_method_return(m, &reply);
        if (r < 0)
                return r;

        r = sd_bus_message_open_container(reply, 'a', "{sv}");
        if (r < 0)
                return r;

        found_interface = !iface ||
                streq(iface, "org.freedesktop.DBus.Properties") ||
                streq(iface, "org.freedesktop.DBus.Peer") ||
                streq(iface, "org.freedesktop.DBus.Introspectable");

        LIST_FOREACH(vtables, c, first) {
                _cleanup_bus_error_free_ sd_bus_error error = SD_BUS_ERROR_NULL;
                void *u;

                if (require_fallback && !c->is_fallback)
                        continue;

                r = node_vtable_get_userdata(bus, m->path, c, &u, &error);
                if (r < 0)
                        return bus_maybe_reply_error(m, r, &error);
                if (bus->nodes_modified)
                        return 0;
                if (r == 0)
                        continue;

                *found_object = true;

                if (iface && !streq(c->interface, iface))
                        continue;
                found_interface = true;

                r = vtable_append_all_properties(bus, reply, m->path, c, u, &error);
                if (r < 0)
                        return bus_maybe_reply_error(m, r, &error);
                if (bus->nodes_modified)
                        return 0;
        }

        if (!found_interface) {
                r = sd_bus_reply_method_errorf(
                                m,
                                SD_BUS_ERROR_UNKNOWN_INTERFACE,
                                "Unknown interface '%s'.", iface);
                if (r < 0)
                        return r;

                return 1;
        }

        r = sd_bus_message_close_container(reply);
        if (r < 0)
                return r;

        r = sd_bus_send(bus, reply, NULL);
        if (r < 0)
                return r;

        return 1;
}

static bool bus_node_with_object_manager(sd_bus *bus, struct node *n) {
        assert(bus);
        assert(n);

        if (n->object_manager)
                return true;

        if (n->parent)
                return bus_node_with_object_manager(bus, n->parent);

        return false;
}

static bool bus_node_exists(
                sd_bus *bus,
                struct node *n,
                const char *path,
                bool require_fallback) {

        struct node_vtable *c;
        struct node_callback *k;

        assert(bus);
        assert(n);
        assert(path);

        /* Tests if there's anything attached directly to this node
         * for the specified path */

        LIST_FOREACH(callbacks, k, n->callbacks) {
                if (require_fallback && !k->is_fallback)
                        continue;

                return true;
        }

        LIST_FOREACH(vtables, c, n->vtables) {
                _cleanup_bus_error_free_ sd_bus_error error = SD_BUS_ERROR_NULL;

                if (require_fallback && !c->is_fallback)
                        continue;

                if (node_vtable_get_userdata(bus, path, c, NULL, &error) > 0)
                        return true;
                if (bus->nodes_modified)
                        return false;
        }

        return !require_fallback && (n->enumerators || n->object_manager);
}

static int process_introspect(
                sd_bus *bus,
                sd_bus_message *m,
                struct node *n,
                bool require_fallback,
                bool *found_object) {

        _cleanup_bus_error_free_ sd_bus_error error = SD_BUS_ERROR_NULL;
        _cleanup_bus_message_unref_ sd_bus_message *reply = NULL;
        _cleanup_set_free_free_ Set *s = NULL;
        const char *previous_interface = NULL;
        struct introspect intro;
        struct node_vtable *c;
        bool empty;
        int r;

        assert(bus);
        assert(m);
        assert(n);
        assert(found_object);

        r = get_child_nodes(bus, m->path, n, &s, &error);
        if (r < 0)
                return bus_maybe_reply_error(m, r, &error);
        if (bus->nodes_modified)
                return 0;

        r = introspect_begin(&intro);
        if (r < 0)
                return r;

        r = introspect_write_default_interfaces(&intro, bus_node_with_object_manager(bus, n));
        if (r < 0)
                return r;

        empty = set_isempty(s);

        LIST_FOREACH(vtables, c, n->vtables) {
                if (require_fallback && !c->is_fallback)
                        continue;

                r = node_vtable_get_userdata(bus, m->path, c, NULL, &error);
                if (r < 0) {
                        r = bus_maybe_reply_error(m, r, &error);
                        goto finish;
                }
                if (bus->nodes_modified) {
                        r = 0;
                        goto finish;
                }
                if (r == 0)
                        continue;

                empty = false;

                if (c->vtable[0].flags & SD_BUS_VTABLE_HIDDEN)
                        continue;

                if (!streq_ptr(previous_interface, c->interface)) {

                        if (previous_interface)
                                fputs(" </interface>\n", intro.f);

                        fprintf(intro.f, " <interface name=\"%s\">\n", c->interface);
                }

                r = introspect_write_interface(&intro, c->vtable);
                if (r < 0)
                        goto finish;

                previous_interface = c->interface;
        }

        if (previous_interface)
                fputs(" </interface>\n", intro.f);

        if (empty) {
                /* Nothing?, let's see if we exist at all, and if not
                 * refuse to do anything */
                r = bus_node_exists(bus, n, m->path, require_fallback);
                if (r < 0)
                        return r;
                if (bus->nodes_modified)
                        return 0;
                if (r == 0)
                        goto finish;
        }

        *found_object = true;

        r = introspect_write_child_nodes(&intro, s, m->path);
        if (r < 0)
                goto finish;

        r = introspect_finish(&intro, bus, m, &reply);
        if (r < 0)
                goto finish;

        r = sd_bus_send(bus, reply, NULL);
        if (r < 0)
                goto finish;

        r = 1;

finish:
        introspect_free(&intro);
        return r;
}

static int object_manager_serialize_path(
                sd_bus *bus,
                sd_bus_message *reply,
                const char *prefix,
                const char *path,
                bool require_fallback,
                sd_bus_error *error) {

        const char *previous_interface = NULL;
        bool found_something = false;
        struct node_vtable *i;
        struct node *n;
        int r;

        assert(bus);
        assert(reply);
        assert(prefix);
        assert(path);
        assert(error);

        n = hashmap_get(bus->nodes, prefix);
        if (!n)
                return 0;

        LIST_FOREACH(vtables, i, n->vtables) {
                void *u;

                if (require_fallback && !i->is_fallback)
                        continue;

                r = node_vtable_get_userdata(bus, path, i, &u, error);
                if (r < 0)
                        return r;
                if (bus->nodes_modified)
                        return 0;
                if (r == 0)
                        continue;

                if (!found_something) {

                        /* Open the object part */

                        r = sd_bus_message_open_container(reply, 'e', "oa{sa{sv}}");
                        if (r < 0)
                                return r;

                        r = sd_bus_message_append(reply, "o", path);
                        if (r < 0)
                                return r;

                        r = sd_bus_message_open_container(reply, 'a', "{sa{sv}}");
                        if (r < 0)
                                return r;

                        found_something = true;
                }

                if (!streq_ptr(previous_interface, i->interface)) {

                        /* Maybe close the previous interface part */

                        if (previous_interface) {
                                r = sd_bus_message_close_container(reply);
                                if (r < 0)
                                        return r;

                                r = sd_bus_message_close_container(reply);
                                if (r < 0)
                                        return r;
                        }

                        /* Open the new interface part */

                        r = sd_bus_message_open_container(reply, 'e', "sa{sv}");
                        if (r < 0)
                                return r;

                        r = sd_bus_message_append(reply, "s", i->interface);
                        if (r < 0)
                                return r;

                        r = sd_bus_message_open_container(reply, 'a', "{sv}");
                        if (r < 0)
                                return r;
                }

                r = vtable_append_all_properties(bus, reply, path, i, u, error);
                if (r < 0)
                        return r;
                if (bus->nodes_modified)
                        return 0;

                previous_interface = i->interface;
        }

        if (previous_interface) {
                r = sd_bus_message_close_container(reply);
                if (r < 0)
                        return r;

                r = sd_bus_message_close_container(reply);
                if (r < 0)
                        return r;
        }

        if (found_something) {
                r = sd_bus_message_close_container(reply);
                if (r < 0)
                        return r;

                r = sd_bus_message_close_container(reply);
                if (r < 0)
                        return r;
        }

        return 1;
}

static int object_manager_serialize_path_and_fallbacks(
                sd_bus *bus,
                sd_bus_message *reply,
                const char *path,
                sd_bus_error *error) {

        char *prefix;
        int r;

        assert(bus);
        assert(reply);
        assert(path);
        assert(error);

        /* First, add all vtables registered for this path */
        r = object_manager_serialize_path(bus, reply, path, path, false, error);
        if (r < 0)
                return r;
        if (bus->nodes_modified)
                return 0;

        /* Second, add fallback vtables registered for any of the prefixes */
        prefix = alloca(strlen(path) + 1);
        OBJECT_PATH_FOREACH_PREFIX(prefix, path) {
                r = object_manager_serialize_path(bus, reply, prefix, path, true, error);
                if (r < 0)
                        return r;
                if (bus->nodes_modified)
                        return 0;
        }

        return 0;
}

static int process_get_managed_objects(
                sd_bus *bus,
                sd_bus_message *m,
                struct node *n,
                bool require_fallback,
                bool *found_object) {

        _cleanup_bus_error_free_ sd_bus_error error = SD_BUS_ERROR_NULL;
        _cleanup_bus_message_unref_ sd_bus_message *reply = NULL;
        _cleanup_set_free_free_ Set *s = NULL;
        bool empty;
        int r;

        assert(bus);
        assert(m);
        assert(n);
        assert(found_object);

        if (!bus_node_with_object_manager(bus, n))
                return 0;

        r = get_child_nodes(bus, m->path, n, &s, &error);
        if (r < 0)
                return r;
        if (bus->nodes_modified)
                return 0;

        r = sd_bus_message_new_method_return(m, &reply);
        if (r < 0)
                return r;

        r = sd_bus_message_open_container(reply, 'a', "{oa{sa{sv}}}");
        if (r < 0)
                return r;

        empty = set_isempty(s);
        if (empty) {
                struct node_vtable *c;

                /* Hmm, so we have no children? Then let's check
                 * whether we exist at all, i.e. whether at least one
                 * vtable exists. */

                LIST_FOREACH(vtables, c, n->vtables) {

                        if (require_fallback && !c->is_fallback)
                                continue;

                        if (r < 0)
                                return r;
                        if (r == 0)
                                continue;

                        empty = false;
                        break;
                }

                if (empty)
                        return 0;
        } else {
                Iterator i;
                char *path;

                SET_FOREACH(path, s, i) {
                        r = object_manager_serialize_path_and_fallbacks(bus, reply, path, &error);
                        if (r < 0)
                                return r;

                        if (bus->nodes_modified)
                                return 0;
                }
        }

        r = sd_bus_message_close_container(reply);
        if (r < 0)
                return r;

        r = sd_bus_send(bus, reply, NULL);
        if (r < 0)
                return r;

        return 1;
}

static int object_find_and_run(
                sd_bus *bus,
                sd_bus_message *m,
                const char *p,
                bool require_fallback,
                bool *found_object) {

        struct node *n;
        struct vtable_member vtable_key, *v;
        int r;

        assert(bus);
        assert(m);
        assert(p);
        assert(found_object);

        n = hashmap_get(bus->nodes, p);
        if (!n)
                return 0;

        /* First, try object callbacks */
        r = node_callbacks_run(bus, m, n->callbacks, require_fallback, found_object);
        if (r != 0)
                return r;
        if (bus->nodes_modified)
                return 0;

        if (!m->interface || !m->member)
                return 0;

        /* Then, look for a known method */
        vtable_key.path = (char*) p;
        vtable_key.interface = m->interface;
        vtable_key.member = m->member;

        v = hashmap_get(bus->vtable_methods, &vtable_key);
        if (v) {
                r = method_callbacks_run(bus, m, v, require_fallback, found_object);
                if (r != 0)
                        return r;
                if (bus->nodes_modified)
                        return 0;
        }

        /* Then, look for a known property */
        if (streq(m->interface, "org.freedesktop.DBus.Properties")) {
                bool get = false;

                get = streq(m->member, "Get");

                if (get || streq(m->member, "Set")) {

                        r = sd_bus_message_rewind(m, true);
                        if (r < 0)
                                return r;

                        vtable_key.path = (char*) p;

                        r = sd_bus_message_read(m, "ss", &vtable_key.interface, &vtable_key.member);
                        if (r < 0)
                                return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_INVALID_ARGS, "Expected interface and member parameters");

                        v = hashmap_get(bus->vtable_properties, &vtable_key);
                        if (v) {
                                r = property_get_set_callbacks_run(bus, m, v, require_fallback, get, found_object);
                                if (r != 0)
                                        return r;
                        }

                } else if (streq(m->member, "GetAll")) {
                        const char *iface;

                        r = sd_bus_message_rewind(m, true);
                        if (r < 0)
                                return r;

                        r = sd_bus_message_read(m, "s", &iface);
                        if (r < 0)
                                return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_INVALID_ARGS, "Expected interface parameter");

                        if (iface[0] == 0)
                                iface = NULL;

                        r = property_get_all_callbacks_run(bus, m, n->vtables, require_fallback, iface, found_object);
                        if (r != 0)
                                return r;
                }

        } else if (sd_bus_message_is_method_call(m, "org.freedesktop.DBus.Introspectable", "Introspect")) {

                if (!isempty(sd_bus_message_get_signature(m, true)))
                        return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_INVALID_ARGS, "Expected no parameters");

                r = process_introspect(bus, m, n, require_fallback, found_object);
                if (r != 0)
                        return r;

        } else if (sd_bus_message_is_method_call(m, "org.freedesktop.DBus.ObjectManager", "GetManagedObjects")) {

                if (!isempty(sd_bus_message_get_signature(m, true)))
                        return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_INVALID_ARGS, "Expected no parameters");

                r = process_get_managed_objects(bus, m, n, require_fallback, found_object);
                if (r != 0)
                        return r;
        }

        if (bus->nodes_modified)
                return 0;

        if (!*found_object) {
                r = bus_node_exists(bus, n, m->path, require_fallback);
                if (r < 0)
                        return r;
                if (r > 0)
                        *found_object = true;
        }

        return 0;
}

int bus_process_object(sd_bus *bus, sd_bus_message *m) {
        int r;
        size_t pl;
        bool found_object = false;

        assert(bus);
        assert(m);

        if (m->header->type != SD_BUS_MESSAGE_METHOD_CALL)
                return 0;

        if (hashmap_isempty(bus->nodes))
                return 0;

        assert(m->path);
        assert(m->member);

        pl = strlen(m->path);
        do {
                char prefix[pl+1];

                bus->nodes_modified = false;

                r = object_find_and_run(bus, m, m->path, false, &found_object);
                if (r != 0)
                        return r;

                /* Look for fallback prefixes */
                OBJECT_PATH_FOREACH_PREFIX(prefix, m->path) {

                        if (bus->nodes_modified)
                                break;

                        r = object_find_and_run(bus, m, prefix, true, &found_object);
                        if (r != 0)
                                return r;
                }

        } while (bus->nodes_modified);

        if (!found_object)
                return 0;

        if (sd_bus_message_is_method_call(m, "org.freedesktop.DBus.Properties", "Get") ||
            sd_bus_message_is_method_call(m, "org.freedesktop.DBus.Properties", "Set"))
                r = sd_bus_reply_method_errorf(
                                m,
                                SD_BUS_ERROR_UNKNOWN_PROPERTY,
                                "Unknown property or interface.");
        else
                r = sd_bus_reply_method_errorf(
                                m,
                                SD_BUS_ERROR_UNKNOWN_METHOD,
                                "Unknown method '%s' or interface '%s'.", m->member, m->interface);

        if (r < 0)
                return r;

        return 1;
}

static struct node *bus_node_allocate(sd_bus *bus, const char *path) {
        struct node *n, *parent;
        const char *e;
        char *s, *p;
        int r;

        assert(bus);
        assert(path);
        assert(path[0] == '/');

        n = hashmap_get(bus->nodes, path);
        if (n)
                return n;

        r = hashmap_ensure_allocated(&bus->nodes, string_hash_func, string_compare_func);
        if (r < 0)
                return NULL;

        s = strdup(path);
        if (!s)
                return NULL;

        if (streq(path, "/"))
                parent = NULL;
        else {
                e = strrchr(path, '/');
                assert(e);

                p = strndupa(path, MAX(1, path - e));

                parent = bus_node_allocate(bus, p);
                if (!parent) {
                        free(s);
                        return NULL;
                }
        }

        n = new0(struct node, 1);
        if (!n)
                return NULL;

        n->parent = parent;
        n->path = s;

        r = hashmap_put(bus->nodes, s, n);
        if (r < 0) {
                free(s);
                free(n);
                return NULL;
        }

        if (parent)
                LIST_PREPEND(siblings, parent->child, n);

        return n;
}

static void bus_node_gc(sd_bus *b, struct node *n) {
        assert(b);

        if (!n)
                return;

        if (n->child ||
            n->callbacks ||
            n->vtables ||
            n->enumerators ||
            n->object_manager)
                return;

        assert(hashmap_remove(b->nodes, n->path) == n);

        if (n->parent)
                LIST_REMOVE(siblings, n->parent->child, n);

        free(n->path);
        bus_node_gc(b, n->parent);
        free(n);
}

static int bus_add_object(
                sd_bus *bus,
                bool fallback,
                const char *path,
                sd_bus_message_handler_t callback,
                void *userdata) {

        struct node_callback *c;
        struct node *n;
        int r;

        assert_return(bus, -EINVAL);
        assert_return(object_path_is_valid(path), -EINVAL);
        assert_return(callback, -EINVAL);
        assert_return(!bus_pid_changed(bus), -ECHILD);

        n = bus_node_allocate(bus, path);
        if (!n)
                return -ENOMEM;

        c = new0(struct node_callback, 1);
        if (!c) {
                r = -ENOMEM;
                goto fail;
        }

        c->node = n;
        c->callback = callback;
        c->userdata = userdata;
        c->is_fallback = fallback;

        LIST_PREPEND(callbacks, n->callbacks, c);
        bus->nodes_modified = true;

        return 0;

fail:
        free(c);
        bus_node_gc(bus, n);
        return r;
}

static int bus_remove_object(
                sd_bus *bus,
                bool fallback,
                const char *path,
                sd_bus_message_handler_t callback,
                void *userdata) {

        struct node_callback *c;
        struct node *n;

        assert_return(bus, -EINVAL);
        assert_return(object_path_is_valid(path), -EINVAL);
        assert_return(callback, -EINVAL);
        assert_return(!bus_pid_changed(bus), -ECHILD);

        n = hashmap_get(bus->nodes, path);
        if (!n)
                return 0;

        LIST_FOREACH(callbacks, c, n->callbacks)
                if (c->callback == callback && c->userdata == userdata && c->is_fallback == fallback)
                        break;
        if (!c)
                return 0;

        LIST_REMOVE(callbacks, n->callbacks, c);
        free(c);

        bus_node_gc(bus, n);
        bus->nodes_modified = true;

        return 1;
}

_public_ int sd_bus_add_object(sd_bus *bus,
                               const char *path,
                               sd_bus_message_handler_t callback,
                               void *userdata) {

        return bus_add_object(bus, false, path, callback, userdata);
}

_public_ int sd_bus_remove_object(sd_bus *bus,
                                  const char *path,
                                  sd_bus_message_handler_t callback,
                                  void *userdata) {

        return bus_remove_object(bus, false, path, callback, userdata);
}

_public_ int sd_bus_add_fallback(sd_bus *bus,
                                 const char *prefix,
                                 sd_bus_message_handler_t callback,
                                 void *userdata) {

        return bus_add_object(bus, true, prefix, callback, userdata);
}

_public_ int sd_bus_remove_fallback(sd_bus *bus,
                                    const char *prefix,
                                    sd_bus_message_handler_t callback,
                                    void *userdata) {

        return bus_remove_object(bus, true, prefix, callback, userdata);
}

static void free_node_vtable(sd_bus *bus, struct node_vtable *w) {
        assert(bus);

        if (!w)
                return;

        if (w->interface && w->node && w->vtable) {
                const sd_bus_vtable *v;

                for (v = w->vtable; v->type != _SD_BUS_VTABLE_END; v++) {
                        struct vtable_member *x = NULL;

                        switch (v->type) {

                        case _SD_BUS_VTABLE_METHOD: {
                                struct vtable_member key;

                                key.path = w->node->path;
                                key.interface = w->interface;
                                key.member = v->x.method.member;

                                x = hashmap_remove(bus->vtable_methods, &key);
                                break;
                        }

                        case _SD_BUS_VTABLE_PROPERTY:
                        case _SD_BUS_VTABLE_WRITABLE_PROPERTY: {
                                struct vtable_member key;

                                key.path = w->node->path;
                                key.interface = w->interface;
                                key.member = v->x.property.member;
                                x = hashmap_remove(bus->vtable_properties, &key);
                                break;
                        }}

                        free(x);
                }
        }

        free(w->interface);
        free(w);
}

static unsigned vtable_member_hash_func(const void *a) {
        const struct vtable_member *m = a;

        assert(m);

        return
                string_hash_func(m->path) ^
                string_hash_func(m->interface) ^
                string_hash_func(m->member);
}

static int vtable_member_compare_func(const void *a, const void *b) {
        const struct vtable_member *x = a, *y = b;
        int r;

        assert(x);
        assert(y);

        r = strcmp(x->path, y->path);
        if (r != 0)
                return r;

        r = strcmp(x->interface, y->interface);
        if (r != 0)
                return r;

        return strcmp(x->member, y->member);
}

static int add_object_vtable_internal(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const sd_bus_vtable *vtable,
                bool fallback,
                sd_bus_object_find_t find,
                void *userdata) {

        struct node_vtable *c = NULL, *i, *existing = NULL;
        const sd_bus_vtable *v;
        struct node *n;
        int r;

        assert_return(bus, -EINVAL);
        assert_return(object_path_is_valid(path), -EINVAL);
        assert_return(interface_name_is_valid(interface), -EINVAL);
        assert_return(vtable, -EINVAL);
        assert_return(vtable[0].type == _SD_BUS_VTABLE_START, -EINVAL);
        assert_return(vtable[0].x.start.element_size == sizeof(struct sd_bus_vtable), -EINVAL);
        assert_return(!bus_pid_changed(bus), -ECHILD);
        assert_return(!streq(interface, "org.freedesktop.DBus.Properties") &&
                      !streq(interface, "org.freedesktop.DBus.Introspectable") &&
                      !streq(interface, "org.freedesktop.DBus.Peer") &&
                      !streq(interface, "org.freedesktop.DBus.ObjectManager"), -EINVAL);

        r = hashmap_ensure_allocated(&bus->vtable_methods, vtable_member_hash_func, vtable_member_compare_func);
        if (r < 0)
                return r;

        r = hashmap_ensure_allocated(&bus->vtable_properties, vtable_member_hash_func, vtable_member_compare_func);
        if (r < 0)
                return r;

        n = bus_node_allocate(bus, path);
        if (!n)
                return -ENOMEM;

        LIST_FOREACH(vtables, i, n->vtables) {
                if (i->is_fallback != fallback) {
                        r = -EPROTOTYPE;
                        goto fail;
                }

                if (streq(i->interface, interface)) {

                        if (i->vtable == vtable) {
                                r = -EEXIST;
                                goto fail;
                        }

                        existing = i;
                }
        }

        c = new0(struct node_vtable, 1);
        if (!c) {
                r = -ENOMEM;
                goto fail;
        }

        c->node = n;
        c->is_fallback = fallback;
        c->vtable = vtable;
        c->userdata = userdata;
        c->find = find;

        c->interface = strdup(interface);
        if (!c->interface) {
                r = -ENOMEM;
                goto fail;
        }

        for (v = c->vtable+1; v->type != _SD_BUS_VTABLE_END; v++) {

                switch (v->type) {

                case _SD_BUS_VTABLE_METHOD: {
                        struct vtable_member *m;

                        if (!member_name_is_valid(v->x.method.member) ||
                            !signature_is_valid(strempty(v->x.method.signature), false) ||
                            !signature_is_valid(strempty(v->x.method.result), false) ||
                            !(v->x.method.handler || (isempty(v->x.method.signature) && isempty(v->x.method.result))) ||
                            v->flags & (SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE|SD_BUS_VTABLE_PROPERTY_INVALIDATE_ONLY)) {
                                r = -EINVAL;
                                goto fail;
                        }

                        m = new0(struct vtable_member, 1);
                        if (!m) {
                                r = -ENOMEM;
                                goto fail;
                        }

                        m->parent = c;
                        m->path = n->path;
                        m->interface = c->interface;
                        m->member = v->x.method.member;
                        m->vtable = v;

                        r = hashmap_put(bus->vtable_methods, m, m);
                        if (r < 0) {
                                free(m);
                                goto fail;
                        }

                        break;
                }

                case _SD_BUS_VTABLE_WRITABLE_PROPERTY:

                        if (!(v->x.property.set || bus_type_is_basic(v->x.property.signature[0]))) {
                                r = -EINVAL;
                                goto fail;
                        }

                        /* Fall through */

                case _SD_BUS_VTABLE_PROPERTY: {
                        struct vtable_member *m;

                        if (!member_name_is_valid(v->x.property.member) ||
                            !signature_is_single(v->x.property.signature, false) ||
                            !(v->x.property.get || bus_type_is_basic(v->x.property.signature[0]) || streq(v->x.property.signature, "as")) ||
                            v->flags & SD_BUS_VTABLE_METHOD_NO_REPLY ||
                            (v->flags & SD_BUS_VTABLE_PROPERTY_INVALIDATE_ONLY && !(v->flags & SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE)) ||
                            (v->flags & SD_BUS_VTABLE_UNPRIVILEGED && v->type == _SD_BUS_VTABLE_PROPERTY)) {
                                r = -EINVAL;
                                goto fail;
                        }


                        m = new0(struct vtable_member, 1);
                        if (!m) {
                                r = -ENOMEM;
                                goto fail;
                        }

                        m->parent = c;
                        m->path = n->path;
                        m->interface = c->interface;
                        m->member = v->x.property.member;
                        m->vtable = v;

                        r = hashmap_put(bus->vtable_properties, m, m);
                        if (r < 0) {
                                free(m);
                                goto fail;
                        }

                        break;
                }

                case _SD_BUS_VTABLE_SIGNAL:

                        if (!member_name_is_valid(v->x.signal.member) ||
                            !signature_is_valid(strempty(v->x.signal.signature), false) ||
                            v->flags & SD_BUS_VTABLE_UNPRIVILEGED) {
                                r = -EINVAL;
                                goto fail;
                        }

                        break;

                default:
                        r = -EINVAL;
                        goto fail;
                }
        }

        LIST_INSERT_AFTER(vtables, n->vtables, existing, c);
        bus->nodes_modified = true;

        return 0;

fail:
        if (c)
                free_node_vtable(bus, c);

        bus_node_gc(bus, n);
        return r;
}

static int remove_object_vtable_internal(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const sd_bus_vtable *vtable,
                bool fallback,
                sd_bus_object_find_t find,
                void *userdata) {

        struct node_vtable *c;
        struct node *n;

        assert_return(bus, -EINVAL);
        assert_return(object_path_is_valid(path), -EINVAL);
        assert_return(interface_name_is_valid(interface), -EINVAL);
        assert_return(!bus_pid_changed(bus), -ECHILD);

        n = hashmap_get(bus->nodes, path);
        if (!n)
                return 0;

        LIST_FOREACH(vtables, c, n->vtables)
                if (streq(c->interface, interface) &&
                    c->is_fallback == fallback &&
                    c->vtable == vtable &&
                    c->find == find &&
                    c->userdata == userdata)
                        break;

        if (!c)
                return 0;

        LIST_REMOVE(vtables, n->vtables, c);

        free_node_vtable(bus, c);
        bus_node_gc(bus, n);

        bus->nodes_modified = true;

        return 1;
}

_public_ int sd_bus_add_object_vtable(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const sd_bus_vtable *vtable,
                void *userdata) {

        return add_object_vtable_internal(bus, path, interface, vtable, false, NULL, userdata);
}

_public_ int sd_bus_remove_object_vtable(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const sd_bus_vtable *vtable,
                void *userdata) {

        return remove_object_vtable_internal(bus, path, interface, vtable, false, NULL, userdata);
}

_public_ int sd_bus_add_fallback_vtable(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const sd_bus_vtable *vtable,
                sd_bus_object_find_t find,
                void *userdata) {

        return add_object_vtable_internal(bus, path, interface, vtable, true, find, userdata);
}

_public_ int sd_bus_remove_fallback_vtable(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const sd_bus_vtable *vtable,
                sd_bus_object_find_t find,
                void *userdata) {

        return remove_object_vtable_internal(bus, path, interface, vtable, true, find, userdata);
}

_public_ int sd_bus_add_node_enumerator(
                sd_bus *bus,
                const char *path,
                sd_bus_node_enumerator_t callback,
                void *userdata) {

        struct node_enumerator *c;
        struct node *n;
        int r;

        assert_return(bus, -EINVAL);
        assert_return(object_path_is_valid(path), -EINVAL);
        assert_return(callback, -EINVAL);
        assert_return(!bus_pid_changed(bus), -ECHILD);

        n = bus_node_allocate(bus, path);
        if (!n)
                return -ENOMEM;

        c = new0(struct node_enumerator, 1);
        if (!c) {
                r = -ENOMEM;
                goto fail;
        }

        c->node = n;
        c->callback = callback;
        c->userdata = userdata;

        LIST_PREPEND(enumerators, n->enumerators, c);

        bus->nodes_modified = true;

        return 0;

fail:
        free(c);
        bus_node_gc(bus, n);
        return r;
}

_public_ int sd_bus_remove_node_enumerator(
                sd_bus *bus,
                const char *path,
                sd_bus_node_enumerator_t callback,
                void *userdata) {

        struct node_enumerator *c;
        struct node *n;

        assert_return(bus, -EINVAL);
        assert_return(object_path_is_valid(path), -EINVAL);
        assert_return(callback, -EINVAL);
        assert_return(!bus_pid_changed(bus), -ECHILD);

        n = hashmap_get(bus->nodes, path);
        if (!n)
                return 0;

        LIST_FOREACH(enumerators, c, n->enumerators)
                if (c->callback == callback && c->userdata == userdata)
                        break;

        if (!c)
                return 0;

        LIST_REMOVE(enumerators, n->enumerators, c);
        free(c);

        bus_node_gc(bus, n);

        bus->nodes_modified = true;

        return 1;
}

static int emit_properties_changed_on_interface(
                sd_bus *bus,
                const char *prefix,
                const char *path,
                const char *interface,
                bool require_fallback,
                char **names) {

        _cleanup_bus_error_free_ sd_bus_error error = SD_BUS_ERROR_NULL;
        _cleanup_bus_message_unref_ sd_bus_message *m = NULL;
        bool has_invalidating = false, has_changing = false;
        struct vtable_member key = {};
        struct node_vtable *c;
        struct node *n;
        char **property;
        void *u = NULL;
        int r;

        assert(bus);
        assert(prefix);
        assert(path);
        assert(interface);

        n = hashmap_get(bus->nodes, prefix);
        if (!n)
                return 0;

        r = sd_bus_message_new_signal(bus, path, "org.freedesktop.DBus.Properties", "PropertiesChanged", &m);
        if (r < 0)
                return r;

        r = sd_bus_message_append(m, "s", interface);
        if (r < 0)
                return r;

        r = sd_bus_message_open_container(m, 'a', "{sv}");
        if (r < 0)
                return r;

        key.path = prefix;
        key.interface = interface;

        LIST_FOREACH(vtables, c, n->vtables) {
                if (require_fallback && !c->is_fallback)
                        continue;

                if (!streq(c->interface, interface))
                        continue;

                r = node_vtable_get_userdata(bus, path, c, &u, &error);
                if (r < 0)
                        return r;
                if (bus->nodes_modified)
                        return 0;
                if (r == 0)
                        continue;

                STRV_FOREACH(property, names) {
                        struct vtable_member *v;

                        assert_return(member_name_is_valid(*property), -EINVAL);

                        key.member = *property;
                        v = hashmap_get(bus->vtable_properties, &key);
                        if (!v)
                                return -ENOENT;

                        /* If there are two vtables for the same
                         * interface, let's handle this property when
                         * we come to that vtable. */
                        if (c != v->parent)
                                continue;

                        assert_return(v->vtable->flags & SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE, -EDOM);

                        if (v->vtable->flags & SD_BUS_VTABLE_PROPERTY_INVALIDATE_ONLY) {
                                has_invalidating = true;
                                continue;
                        }

                        has_changing = true;

                        r = sd_bus_message_open_container(m, 'e', "sv");
                        if (r < 0)
                                return r;

                        r = sd_bus_message_append(m, "s", *property);
                        if (r < 0)
                                return r;

                        r = sd_bus_message_open_container(m, 'v', v->vtable->x.property.signature);
                        if (r < 0)
                                return r;

                        r = invoke_property_get(bus, v->vtable, m->path, interface, *property, m, vtable_property_convert_userdata(v->vtable, u), &error);
                        if (r < 0)
                                return r;
                        if (bus->nodes_modified)
                                return 0;

                        r = sd_bus_message_close_container(m);
                        if (r < 0)
                                return r;

                        r = sd_bus_message_close_container(m);
                        if (r < 0)
                                return r;
                }
        }

        if (!has_invalidating && !has_changing)
                return 0;

        r = sd_bus_message_close_container(m);
        if (r < 0)
                return r;

        r = sd_bus_message_open_container(m, 'a', "s");
        if (r < 0)
                return r;

        if (has_invalidating) {
                LIST_FOREACH(vtables, c, n->vtables) {
                        if (require_fallback && !c->is_fallback)
                                continue;

                        if (!streq(c->interface, interface))
                                continue;

                        r = node_vtable_get_userdata(bus, path, c, &u, &error);
                        if (r < 0)
                                return r;
                        if (bus->nodes_modified)
                                return 0;
                        if (r == 0)
                                continue;

                        STRV_FOREACH(property, names) {
                                struct vtable_member *v;

                                key.member = *property;
                                assert_se(v = hashmap_get(bus->vtable_properties, &key));
                                assert(c == v->parent);

                                if (!(v->vtable->flags & SD_BUS_VTABLE_PROPERTY_INVALIDATE_ONLY))
                                        continue;

                                r = sd_bus_message_append(m, "s", *property);
                                if (r < 0)
                                        return r;
                        }
                }
        }

        r = sd_bus_message_close_container(m);
        if (r < 0)
                return r;

        r = sd_bus_send(bus, m, NULL);
        if (r < 0)
                return r;

        return 1;
}

_public_ int sd_bus_emit_properties_changed_strv(
                sd_bus *bus,
                const char *path,
                const char *interface,
                char **names) {

        BUS_DONT_DESTROY(bus);
        char *prefix;
        int r;

        assert_return(bus, -EINVAL);
        assert_return(object_path_is_valid(path), -EINVAL);
        assert_return(interface_name_is_valid(interface), -EINVAL);
        assert_return(BUS_IS_OPEN(bus->state), -ENOTCONN);
        assert_return(!bus_pid_changed(bus), -ECHILD);

        if (strv_isempty(names))
                return 0;

        do {
                bus->nodes_modified = false;

                r = emit_properties_changed_on_interface(bus, path, path, interface, false, names);
                if (r != 0)
                        return r;
                if (bus->nodes_modified)
                        continue;

                prefix = alloca(strlen(path) + 1);
                OBJECT_PATH_FOREACH_PREFIX(prefix, path) {
                        r = emit_properties_changed_on_interface(bus, prefix, path, interface, true, names);
                        if (r != 0)
                                return r;
                        if (bus->nodes_modified)
                                break;
                }

        } while (bus->nodes_modified);

        return -ENOENT;
}

_public_ int sd_bus_emit_properties_changed(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *name, ...)  {

        char **names;

        assert_return(bus, -EINVAL);
        assert_return(object_path_is_valid(path), -EINVAL);
        assert_return(interface_name_is_valid(interface), -EINVAL);
        assert_return(BUS_IS_OPEN(bus->state), -ENOTCONN);
        assert_return(!bus_pid_changed(bus), -ECHILD);

        if (!name)
                return 0;

        names = strv_from_stdarg_alloca(name);

        return sd_bus_emit_properties_changed_strv(bus, path, interface, names);
}

static int interfaces_added_append_one_prefix(
                sd_bus *bus,
                sd_bus_message *m,
                const char *prefix,
                const char *path,
                const char *interface,
                bool require_fallback) {

        _cleanup_bus_error_free_ sd_bus_error error = SD_BUS_ERROR_NULL;
        bool found_interface = false;
        struct node_vtable *c;
        struct node *n;
        void *u = NULL;
        int r;

        assert(bus);
        assert(m);
        assert(prefix);
        assert(path);
        assert(interface);

        n = hashmap_get(bus->nodes, prefix);
        if (!n)
                return 0;

        LIST_FOREACH(vtables, c, n->vtables) {
                if (require_fallback && !c->is_fallback)
                        continue;

                if (!streq(c->interface, interface))
                        continue;

                r = node_vtable_get_userdata(bus, path, c, &u, &error);
                if (r < 0)
                        return r;
                if (bus->nodes_modified)
                        return 0;
                if (r == 0)
                        continue;

                if (!found_interface) {
                        r = sd_bus_message_append_basic(m, 's', interface);
                        if (r < 0)
                                return r;

                        r = sd_bus_message_open_container(m, 'a', "{sv}");
                        if (r < 0)
                                return r;

                        found_interface = true;
                }

                r = vtable_append_all_properties(bus, m, path, c, u, &error);
                if (r < 0)
                        return r;
                if (bus->nodes_modified)
                        return 0;
        }

        if (found_interface) {
                r = sd_bus_message_close_container(m);
                if (r < 0)
                        return r;
        }

        return found_interface;
}

static int interfaces_added_append_one(
                sd_bus *bus,
                sd_bus_message *m,
                const char *path,
                const char *interface) {

        char *prefix;
        int r;

        assert(bus);
        assert(m);
        assert(path);
        assert(interface);

        r = interfaces_added_append_one_prefix(bus, m, path, path, interface, false);
        if (r != 0)
                return r;
        if (bus->nodes_modified)
                return 0;

        prefix = alloca(strlen(path) + 1);
        OBJECT_PATH_FOREACH_PREFIX(prefix, path) {
                r = interfaces_added_append_one_prefix(bus, m, prefix, path, interface, true);
                if (r != 0)
                        return r;
                if (bus->nodes_modified)
                        return 0;
        }

        return -ENOENT;
}

_public_ int sd_bus_emit_interfaces_added_strv(sd_bus *bus, const char *path, char **interfaces) {
        BUS_DONT_DESTROY(bus);

        _cleanup_bus_message_unref_ sd_bus_message *m = NULL;
        char **i;
        int r;

        assert_return(bus, -EINVAL);
        assert_return(object_path_is_valid(path), -EINVAL);
        assert_return(BUS_IS_OPEN(bus->state), -ENOTCONN);
        assert_return(!bus_pid_changed(bus), -ECHILD);

        if (strv_isempty(interfaces))
                return 0;

        do {
                bus->nodes_modified = false;

                if (m)
                        m = sd_bus_message_unref(m);

                r = sd_bus_message_new_signal(bus, path, "org.freedesktop.DBus.ObjectManager", "InterfacesAdded", &m);
                if (r < 0)
                        return r;

                r = sd_bus_message_append_basic(m, 'o', path);
                if (r < 0)
                        return r;

                r = sd_bus_message_open_container(m, 'a', "{sa{sv}}");
                if (r < 0)
                        return r;

                STRV_FOREACH(i, interfaces) {
                        assert_return(interface_name_is_valid(*i), -EINVAL);

                        r = sd_bus_message_open_container(m, 'e', "sa{sv}");
                        if (r < 0)
                                return r;

                        r = interfaces_added_append_one(bus, m, path, *i);
                        if (r < 0)
                                return r;

                        if (bus->nodes_modified)
                                break;

                        r = sd_bus_message_close_container(m);
                        if (r < 0)
                                return r;
                }

                if (bus->nodes_modified)
                        continue;

                r = sd_bus_message_close_container(m);
                if (r < 0)
                        return r;

        } while (bus->nodes_modified);

        return sd_bus_send(bus, m, NULL);
}

_public_ int sd_bus_emit_interfaces_added(sd_bus *bus, const char *path, const char *interface, ...) {
        char **interfaces;

        assert_return(bus, -EINVAL);
        assert_return(object_path_is_valid(path), -EINVAL);
        assert_return(BUS_IS_OPEN(bus->state), -ENOTCONN);
        assert_return(!bus_pid_changed(bus), -ECHILD);

        interfaces = strv_from_stdarg_alloca(interface);

        return sd_bus_emit_interfaces_added_strv(bus, path, interfaces);
}

_public_ int sd_bus_emit_interfaces_removed_strv(sd_bus *bus, const char *path, char **interfaces) {
        _cleanup_bus_message_unref_ sd_bus_message *m = NULL;
        int r;

        assert_return(bus, -EINVAL);
        assert_return(object_path_is_valid(path), -EINVAL);
        assert_return(BUS_IS_OPEN(bus->state), -ENOTCONN);
        assert_return(!bus_pid_changed(bus), -ECHILD);

        if (strv_isempty(interfaces))
                return 0;

        r = sd_bus_message_new_signal(bus, path, "org.freedesktop.DBus.ObjectManager", "InterfacesRemoved", &m);
        if (r < 0)
                return r;

        r = sd_bus_message_append_basic(m, 'o', path);
        if (r < 0)
                return r;

        r = sd_bus_message_append_strv(m, interfaces);
        if (r < 0)
                return r;

        return sd_bus_send(bus, m, NULL);
}

_public_ int sd_bus_emit_interfaces_removed(sd_bus *bus, const char *path, const char *interface, ...) {
        char **interfaces;

        assert_return(bus, -EINVAL);
        assert_return(object_path_is_valid(path), -EINVAL);
        assert_return(BUS_IS_OPEN(bus->state), -ENOTCONN);
        assert_return(!bus_pid_changed(bus), -ECHILD);

        interfaces = strv_from_stdarg_alloca(interface);

        return sd_bus_emit_interfaces_removed_strv(bus, path, interfaces);
}

_public_ int sd_bus_add_object_manager(sd_bus *bus, const char *path) {
        struct node *n;

        assert_return(bus, -EINVAL);
        assert_return(object_path_is_valid(path), -EINVAL);
        assert_return(!bus_pid_changed(bus), -ECHILD);

        n = bus_node_allocate(bus, path);
        if (!n)
                return -ENOMEM;

        n->object_manager = true;
        bus->nodes_modified = true;
        return 0;
}

_public_ int sd_bus_remove_object_manager(sd_bus *bus, const char *path) {
        struct node *n;

        assert_return(bus, -EINVAL);
        assert_return(object_path_is_valid(path), -EINVAL);
        assert_return(!bus_pid_changed(bus), -ECHILD);

        n = hashmap_get(bus->nodes, path);
        if (!n)
                return 0;

        if (!n->object_manager)
                return 0;

        n->object_manager = false;
        bus->nodes_modified = true;
        bus_node_gc(bus, n);

        return 1;
}
