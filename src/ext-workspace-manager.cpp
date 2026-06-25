#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

#include <wayfire/core.hpp>
#include <wayfire/option-wrapper.hpp>
#include <wayfire/output-layout.hpp>
#include <wayfire/output.hpp>
#include <wayfire/plugin.hpp>
#include <wayfire/signal-definitions.hpp>
#include <wayfire/toplevel-view.hpp>
#include <wayfire/view.hpp>
#include <wayfire/workspace-set.hpp>
#include <wayfire/nonstd/wlroots.hpp>

#include "ext-workspace-v1-protocol.h"

namespace
{
struct workspace_key_t
{
    uint64_t wset = 0;
    int x = 0;
    int y = 0;

    bool operator <(const workspace_key_t& other) const
    {
        if (wset != other.wset)
        {
            return wset < other.wset;
        }

        if (x != other.x)
        {
            return x < other.x;
        }

        return y < other.y;
    }
};

static std::string workspace_id(const workspace_key_t& key)
{
    std::ostringstream out;
    out << "wset-" << key.wset << ":" << key.x << "," << key.y;
    return out.str();
}

static std::string workspace_name(const workspace_key_t& key, wf::dimensions_t grid)
{
    return std::to_string(key.y * grid.width + key.x + 1);
}

static std::string trim_name(const std::string& name)
{
    auto first = name.find_first_not_of(" \t");
    if (first == std::string::npos)
    {
        return "";
    }

    auto last = name.find_last_not_of(" \t");
    return name.substr(first, last - first + 1);
}

static std::vector<std::string> split_workspace_names(const std::string& names)
{
    std::vector<std::string> result;
    std::stringstream stream(names);
    std::string name;
    while (std::getline(stream, name, ','))
    {
        result.push_back(trim_name(name));
    }

    return result;
}

class ext_workspace_manager_plugin_t;
struct manager_client_t;

static uint32_t workspace_state(ext_workspace_manager_plugin_t *plugin,
    std::shared_ptr<wf::workspace_set_t> wset, const workspace_key_t& key);

static uint32_t workspace_base_state(std::shared_ptr<wf::workspace_set_t> wset,
    const workspace_key_t& key)
{
    auto current = wset->get_current_workspace();
    if ((current.x == key.x) && (current.y == key.y))
    {
        return EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE;
    }

    return 0;
}

static uint32_t workspace_capabilities(std::shared_ptr<wf::workspace_set_t> wset,
    const workspace_key_t& key)
{
    uint32_t capabilities = EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ACTIVATE;
    auto grid = wset->get_workspace_grid_size();
    if (((key.x == grid.width - 1) && (grid.width > 1)) ||
        ((key.y == grid.height - 1) && (grid.height > 1)))
    {
        capabilities |= EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_REMOVE;
    }

    return capabilities;
}

static wl_resource *find_output_resource_for_client(wlr_output *output, wl_client *client)
{
    if (!output)
    {
        return nullptr;
    }

    wl_resource *resource = nullptr;
    wl_resource_for_each(resource, &output->resources)
    {
        if (wl_resource_get_client(resource) == client)
        {
            return resource;
        }
    }

    return nullptr;
}

struct workspace_handle_t
{
    manager_client_t *client = nullptr;
    wl_resource *resource = nullptr;
    workspace_key_t key;
    bool sent_id = false;
    std::string sent_name;
    uint32_t sent_state = 0;
    uint32_t sent_capabilities = 0;
    bool sent_details = false;
    bool removed = false;
};

struct group_handle_t
{
    manager_client_t *client = nullptr;
    wl_resource *resource = nullptr;
    uint64_t wset_index = 0;
    std::set<std::string> output_names;
    std::set<workspace_key_t> workspaces;
    bool removed = false;
};

struct current_group_t
{
    std::shared_ptr<wf::workspace_set_t> wset;
    std::set<wf::output_t*> outputs;
};

struct current_snapshot_t
{
    std::map<uint64_t, current_group_t> groups;
    std::map<workspace_key_t, std::shared_ptr<wf::workspace_set_t>> workspaces;
};

struct urgent_view_t
{
    wayfire_toplevel_view view;
    wf::signal::connection_t<wf::view_activated_state_signal> activated;
};

static current_snapshot_t get_current_snapshot()
{
    current_snapshot_t snapshot;

    for (auto *output : wf::get_core().output_layout->get_outputs())
    {
        if (!output || !output->wset())
        {
            continue;
        }

        auto wset = output->wset();
        auto wset_index = wset->get_index();
        auto& group = snapshot.groups[wset_index];
        group.wset = wset;
        group.outputs.insert(output);

        auto grid = wset->get_workspace_grid_size();
        for (int y = 0; y < grid.height; y++)
        {
            for (int x = 0; x < grid.width; x++)
            {
                workspace_key_t key{wset_index, x, y};
                snapshot.workspaces[key] = wset;
            }
        }
    }

    return snapshot;
}

struct manager_client_t
{
    ext_workspace_manager_plugin_t *plugin = nullptr;
    wl_resource *resource = nullptr;
    bool stopped = false;

    std::map<uint64_t, std::unique_ptr<group_handle_t>> groups;
    std::map<workspace_key_t, std::unique_ptr<workspace_handle_t>> workspaces;
    std::vector<std::unique_ptr<group_handle_t>> retired_groups;
    std::vector<std::unique_ptr<workspace_handle_t>> retired_workspaces;
    std::map<uint64_t, wf::point_t> pending_activation;
    std::vector<std::pair<uint64_t, std::string>> pending_creation;
    std::vector<workspace_key_t> pending_removal;

    manager_client_t(ext_workspace_manager_plugin_t *plugin, wl_resource *resource) :
        plugin(plugin), resource(resource)
    {}

    wl_client *get_client() const
    {
        return wl_resource_get_client(resource);
    }

    int version() const
    {
        return wl_resource_get_version(resource);
    }

    void send_done()
    {
        if (resource && !stopped)
        {
            ext_workspace_manager_v1_send_done(resource);
        }
    }

    void sync();
    void commit();
    void finish();
    void forget_workspace(workspace_handle_t *handle);
    void forget_group(group_handle_t *handle);
    bool has_live_resources() const;
    void maybe_reap();

    group_handle_t *ensure_group(uint64_t wset_index);
    workspace_handle_t *ensure_workspace(const workspace_key_t& key);
    void send_workspace_details(workspace_handle_t *handle, std::shared_ptr<wf::workspace_set_t> wset);
    void sync_group_outputs(group_handle_t *handle, const std::set<wf::output_t*>& outputs);
    void sync_group_workspaces(group_handle_t *handle, const std::set<workspace_key_t>& keys);
};

static void handle_workspace_destroy(wl_client*, wl_resource *resource)
{
    wl_resource_destroy(resource);
}

static void handle_workspace_activate(wl_client*, wl_resource *resource)
{
    auto *handle = static_cast<workspace_handle_t*>(wl_resource_get_user_data(resource));
    if (!handle || handle->removed || !handle->client || handle->client->stopped)
    {
        return;
    }

    handle->client->pending_activation[handle->key.wset] = {handle->key.x, handle->key.y};
}

static void handle_workspace_noop(wl_client*, wl_resource*)
{}

static void handle_workspace_assign(wl_client*, wl_resource*, wl_resource*)
{}

static void handle_workspace_remove(wl_client*, wl_resource *resource)
{
    auto *handle = static_cast<workspace_handle_t*>(wl_resource_get_user_data(resource));
    if (!handle || handle->removed || !handle->client || handle->client->stopped)
    {
        return;
    }

    handle->client->pending_removal.push_back(handle->key);
}

static const struct ext_workspace_handle_v1_interface workspace_impl = {
    .destroy = handle_workspace_destroy,
    .activate = handle_workspace_activate,
    .deactivate = handle_workspace_noop,
    .assign = handle_workspace_assign,
    .remove = handle_workspace_remove,
};

static void workspace_resource_destroyed(wl_resource *resource)
{
    auto *handle = static_cast<workspace_handle_t*>(wl_resource_get_user_data(resource));
    if (handle && handle->client)
    {
        auto *client = handle->client;
        handle->resource = nullptr;
        client->forget_workspace(handle);
        client->maybe_reap();
    }
}

static void handle_group_create_workspace(wl_client*, wl_resource *resource, const char *name)
{
    auto *handle = static_cast<group_handle_t*>(wl_resource_get_user_data(resource));
    if (!handle || handle->removed || !handle->client || handle->client->stopped)
    {
        return;
    }

    handle->client->pending_creation.push_back({handle->wset_index, name ? name : ""});
}

static void handle_group_destroy(wl_client*, wl_resource *resource)
{
    wl_resource_destroy(resource);
}

static const struct ext_workspace_group_handle_v1_interface group_impl = {
    .create_workspace = handle_group_create_workspace,
    .destroy = handle_group_destroy,
};

static void group_resource_destroyed(wl_resource *resource)
{
    auto *handle = static_cast<group_handle_t*>(wl_resource_get_user_data(resource));
    if (handle && handle->client)
    {
        auto *client = handle->client;
        handle->resource = nullptr;
        client->forget_group(handle);
        client->maybe_reap();
    }
}

static void handle_manager_commit(wl_client*, wl_resource *resource)
{
    auto *client = static_cast<manager_client_t*>(wl_resource_get_user_data(resource));
    if (client)
    {
        client->commit();
    }
}

static void handle_manager_stop(wl_client*, wl_resource *resource)
{
    auto *client = static_cast<manager_client_t*>(wl_resource_get_user_data(resource));
    if (client)
    {
        client->finish();
    }
}

static const struct ext_workspace_manager_v1_interface manager_impl = {
    .commit = handle_manager_commit,
    .stop = handle_manager_stop,
};

class ext_workspace_manager_plugin_t : public wf::plugin_interface_t
{
  public:
    void init() override
    {
        workspace_names.set_callback([=] () { broadcast_sync(); });

        on_view_hints_changed = [=] (wf::view_hints_changed_signal *ev)
        {
            if (ev->demands_attention)
            {
                track_urgent_view(wf::toplevel_cast(ev->view));
            } else
            {
                untrack_urgent_view(ev->view);
            }

            broadcast_sync();
        };
        wf::get_core().connect(&on_view_hints_changed);

        on_view_system_bell = [=] (wf::view_system_bell_signal *ev)
        {
            if (auto toplevel = wf::toplevel_cast(ev->view))
            {
                track_urgent_view(toplevel);
                broadcast_sync();
            }
        };
        wf::get_core().connect(&on_view_system_bell);

        on_view_unmapped = [=] (wf::view_unmapped_signal *ev)
        {
            untrack_urgent_view(ev->view);
        };
        wf::get_core().connect(&on_view_unmapped);

        on_view_moved_to_wset = [=] (wf::view_moved_to_wset_signal*)
        {
            broadcast_sync();
        };
        wf::get_core().connect(&on_view_moved_to_wset);

        global = wl_global_create(wf::get_core().display, &ext_workspace_manager_v1_interface, 1, this,
            bind_manager);
        wf::get_core().output_layout->connect(&on_output_added);
        wf::get_core().output_layout->connect(&on_output_removed);

        for (auto *output : wf::get_core().output_layout->get_outputs())
        {
            track_output(output);
        }
    }

    void fini() override
    {
        urgent_views.clear();
        clients.clear();
        retired_clients.clear();
        tracked_outputs.clear();

        if (global)
        {
            wl_global_destroy(global);
            global = nullptr;
        }
    }

    bool is_unloadable() override
    {
        return false;
    }

    void remove_client(manager_client_t *client)
    {
        auto it = std::remove_if(clients.begin(), clients.end(),
            [=] (const auto& item) { return item.get() == client; });
        clients.erase(it, clients.end());
    }

    void retire_client(manager_client_t *client)
    {
        auto it = std::find_if(clients.begin(), clients.end(),
            [=] (const auto& item) { return item.get() == client; });
        if (it == clients.end())
        {
            return;
        }

        auto retired = std::move(*it);
        clients.erase(it);
        if (retired->has_live_resources())
        {
            retired_clients.push_back(std::move(retired));
        }
    }

    void reap_retired_clients()
    {
        auto it = std::remove_if(retired_clients.begin(), retired_clients.end(),
            [] (const auto& client) { return !client->has_live_resources(); });
        retired_clients.erase(it, retired_clients.end());
    }

    void set_workspace_name(const workspace_key_t& key, const std::string& name)
    {
        auto trimmed = trim_name(name);
        if (trimmed.empty())
        {
            return;
        }

        created_workspace_names[key] = trimmed;
    }

    void forget_workspace_names(uint64_t wset_index, wf::dimensions_t grid)
    {
        auto it = created_workspace_names.begin();
        while (it != created_workspace_names.end())
        {
            if ((it->first.wset == wset_index) &&
                ((it->first.x >= grid.width) || (it->first.y >= grid.height)))
            {
                it = created_workspace_names.erase(it);
            } else
            {
                ++it;
            }
        }
    }

    std::string get_workspace_name(const workspace_key_t& key, wf::dimensions_t grid)
    {
        auto names = split_workspace_names(workspace_names);
        auto index = key.y * grid.width + key.x;
        if ((index >= 0) && (static_cast<size_t>(index) < names.size()) && !names[index].empty())
        {
            return names[index];
        }

        auto created = created_workspace_names.find(key);
        if (created != created_workspace_names.end())
        {
            return created->second;
        }

        return workspace_name(key, grid);
    }

    void broadcast_sync()
    {
        for (auto& client : clients)
        {
            client->sync();
        }
    }

    void track_urgent_view(wayfire_toplevel_view view)
    {
        if (!view || urgent_views.count(view))
        {
            return;
        }

        auto urgent = std::make_unique<urgent_view_t>();
        urgent->view = view;
        urgent->activated = [=] (wf::view_activated_state_signal *ev)
        {
            if (ev->view && ev->view->activated)
            {
                untrack_urgent_view(ev->view);
            }
        };
        view->connect(&urgent->activated);
        urgent_views[view] = std::move(urgent);
    }

    void untrack_urgent_view(wayfire_view view)
    {
        if (auto toplevel = wf::toplevel_cast(view))
        {
            if (urgent_views.erase(toplevel))
            {
                broadcast_sync();
            }
        }
    }

    bool workspace_is_urgent(const workspace_key_t& key)
    {
        for (const auto& [view, _] : urgent_views)
        {
            if (!view || !view->get_wset() || !view->is_mapped() || view->activated)
            {
                continue;
            }

            auto wset = view->get_wset();
            auto workspace = wset->get_view_main_workspace(view);
            if ((static_cast<uint64_t>(wset->get_index()) == key.wset) &&
                (workspace.x == key.x) && (workspace.y == key.y))
            {
                return true;
            }
        }

        return false;
    }

  private:
    struct tracked_output_t
    {
        wf::output_t *output = nullptr;
        wf::signal::connection_t<wf::workspace_changed_signal> workspace_changed;
        wf::signal::connection_t<wf::workspace_set_changed_signal> wset_changed;
        wf::signal::connection_t<wf::workspace_grid_changed_signal> grid_changed;
        wf::signal::connection_t<wf::view_change_workspace_signal> view_workspace_changed;
    };

    wl_global *global = nullptr;
    wf::option_wrapper_t<std::string> workspace_names{"ext-workspace-manager/names"};
    std::map<workspace_key_t, std::string> created_workspace_names;
    std::map<wayfire_toplevel_view, std::unique_ptr<urgent_view_t>> urgent_views;
    std::vector<std::unique_ptr<manager_client_t>> clients;
    std::vector<std::unique_ptr<manager_client_t>> retired_clients;
    std::vector<std::unique_ptr<tracked_output_t>> tracked_outputs;
    wf::signal::connection_t<wf::view_hints_changed_signal> on_view_hints_changed;
    wf::signal::connection_t<wf::view_system_bell_signal> on_view_system_bell;
    wf::signal::connection_t<wf::view_unmapped_signal> on_view_unmapped;
    wf::signal::connection_t<wf::view_moved_to_wset_signal> on_view_moved_to_wset;

    static void bind_manager(wl_client *client, void *data, uint32_t version, uint32_t id)
    {
        auto *self = static_cast<ext_workspace_manager_plugin_t*>(data);
        auto *resource = wl_resource_create(client, &ext_workspace_manager_v1_interface, std::min(version, 1u), id);
        if (!resource)
        {
            wl_client_post_no_memory(client);
            return;
        }

        auto manager = std::make_unique<manager_client_t>(self, resource);
        auto *manager_ptr = manager.get();
        wl_resource_set_implementation(resource, &manager_impl, manager_ptr, manager_resource_destroyed);
        self->clients.push_back(std::move(manager));
        manager_ptr->sync();
    }

    static void manager_resource_destroyed(wl_resource *resource)
    {
        auto *client = static_cast<manager_client_t*>(wl_resource_get_user_data(resource));
        if (client)
        {
            client->resource = nullptr;
            client->stopped = true;
            client->plugin->retire_client(client);
        }
    }

    void track_output(wf::output_t *output)
    {
        if (!output || find_tracked(output))
        {
            return;
        }

        auto tracked = std::make_unique<tracked_output_t>();
        tracked->output = output;

        tracked->workspace_changed = [=] (wf::workspace_changed_signal*)
        {
            broadcast_sync();
        };
        output->connect(&tracked->workspace_changed);

        tracked->wset_changed = [=] (wf::workspace_set_changed_signal *ev)
        {
            if (auto *entry = find_tracked(ev->output))
            {
                entry->grid_changed.disconnect();
                if (ev->new_wset)
                {
                    ev->new_wset->connect(&entry->grid_changed);
                }
            }

            broadcast_sync();
        };
        output->connect(&tracked->wset_changed);

        tracked->grid_changed = [=] (wf::workspace_grid_changed_signal*)
        {
            broadcast_sync();
        };
        output->wset()->connect(&tracked->grid_changed);

        tracked->view_workspace_changed = [=] (wf::view_change_workspace_signal*)
        {
            broadcast_sync();
        };
        output->connect(&tracked->view_workspace_changed);

        tracked_outputs.push_back(std::move(tracked));
    }

    tracked_output_t *find_tracked(wf::output_t *output)
    {
        for (auto& tracked : tracked_outputs)
        {
            if (tracked->output == output)
            {
                return tracked.get();
            }
        }

        return nullptr;
    }

    void untrack_output(wf::output_t *output)
    {
        auto it = std::remove_if(tracked_outputs.begin(), tracked_outputs.end(),
            [=] (const auto& tracked) { return tracked->output == output; });
        tracked_outputs.erase(it, tracked_outputs.end());
    }

    wf::signal::connection_t<wf::output_added_signal> on_output_added = [=] (wf::output_added_signal *ev)
    {
        track_output(ev->output);
        broadcast_sync();
    };

    wf::signal::connection_t<wf::output_removed_signal> on_output_removed = [=] (wf::output_removed_signal *ev)
    {
        untrack_output(ev->output);
        broadcast_sync();
    };
};

static uint32_t workspace_state(ext_workspace_manager_plugin_t *plugin,
    std::shared_ptr<wf::workspace_set_t> wset, const workspace_key_t& key)
{
    auto state = workspace_base_state(wset, key);
    if (plugin && plugin->workspace_is_urgent(key))
    {
        state |= EXT_WORKSPACE_HANDLE_V1_STATE_URGENT;
    }

    return state;
}

bool manager_client_t::has_live_resources() const
{
    return resource || !groups.empty() || !workspaces.empty() ||
        !retired_groups.empty() || !retired_workspaces.empty();
}

void manager_client_t::maybe_reap()
{
    if (plugin)
    {
        plugin->reap_retired_clients();
    }
}

group_handle_t *manager_client_t::ensure_group(uint64_t wset_index)
{
    auto existing = groups.find(wset_index);
    if (existing != groups.end())
    {
        return existing->second.get();
    }

    auto resource = wl_resource_create(get_client(), &ext_workspace_group_handle_v1_interface, version(), 0);
    if (!resource)
    {
        wl_resource_post_no_memory(this->resource);
        return nullptr;
    }

    auto handle = std::make_unique<group_handle_t>();
    handle->client = this;
    handle->resource = resource;
    handle->wset_index = wset_index;

    auto *handle_ptr = handle.get();
    wl_resource_set_implementation(resource, &group_impl, handle_ptr, group_resource_destroyed);
    groups[wset_index] = std::move(handle);

    ext_workspace_manager_v1_send_workspace_group(this->resource, resource);
    ext_workspace_group_handle_v1_send_capabilities(resource,
        EXT_WORKSPACE_GROUP_HANDLE_V1_GROUP_CAPABILITIES_CREATE_WORKSPACE);
    return handle_ptr;
}

workspace_handle_t *manager_client_t::ensure_workspace(const workspace_key_t& key)
{
    auto existing = workspaces.find(key);
    if (existing != workspaces.end())
    {
        return existing->second.get();
    }

    auto resource = wl_resource_create(get_client(), &ext_workspace_handle_v1_interface, version(), 0);
    if (!resource)
    {
        wl_resource_post_no_memory(this->resource);
        return nullptr;
    }

    auto handle = std::make_unique<workspace_handle_t>();
    handle->client = this;
    handle->resource = resource;
    handle->key = key;

    auto *handle_ptr = handle.get();
    wl_resource_set_implementation(resource, &workspace_impl, handle_ptr, workspace_resource_destroyed);
    workspaces[key] = std::move(handle);

    ext_workspace_manager_v1_send_workspace(this->resource, resource);
    return handle_ptr;
}

void manager_client_t::send_workspace_details(workspace_handle_t *handle,
    std::shared_ptr<wf::workspace_set_t> wset)
{
    if (!handle || !handle->resource || !wset)
    {
        return;
    }

    auto grid = wset->get_workspace_grid_size();
    auto id = workspace_id(handle->key);
    auto name = handle->client->plugin->get_workspace_name(handle->key, grid);
    auto state = workspace_state(handle->client->plugin, wset, handle->key);
    auto capabilities = workspace_capabilities(wset, handle->key);

    if (!handle->sent_id)
    {
        ext_workspace_handle_v1_send_id(handle->resource, id.c_str());
        handle->sent_id = true;
    }

    if (!handle->sent_details || (handle->sent_name != name))
    {
        ext_workspace_handle_v1_send_name(handle->resource, name.c_str());
        handle->sent_name = name;
    }

    if (!handle->sent_details)
    {
        wl_array coordinates;
        wl_array_init(&coordinates);
        auto *x = static_cast<uint32_t*>(wl_array_add(&coordinates, sizeof(uint32_t)));
        auto *y = static_cast<uint32_t*>(wl_array_add(&coordinates, sizeof(uint32_t)));
        if (x && y)
        {
            *x = handle->key.x;
            *y = handle->key.y;
            ext_workspace_handle_v1_send_coordinates(handle->resource, &coordinates);
        }

        wl_array_release(&coordinates);
    }

    if (!handle->sent_details || (handle->sent_state != state))
    {
        ext_workspace_handle_v1_send_state(handle->resource, state);
        handle->sent_state = state;
    }

    if (!handle->sent_details || (handle->sent_capabilities != capabilities))
    {
        ext_workspace_handle_v1_send_capabilities(handle->resource, capabilities);
        handle->sent_capabilities = capabilities;
    }

    handle->sent_details = true;
}

void manager_client_t::sync_group_outputs(group_handle_t *handle, const std::set<wf::output_t*>& outputs)
{
    if (!handle || !handle->resource)
    {
        return;
    }

    std::set<std::string> next_names;
    for (auto *output : outputs)
    {
        if (!output || !output->handle)
        {
            continue;
        }

        next_names.insert(output->handle->name);
        if (!handle->output_names.count(output->handle->name))
        {
            if (auto *output_resource = find_output_resource_for_client(output->handle, get_client()))
            {
                ext_workspace_group_handle_v1_send_output_enter(handle->resource, output_resource);
            }
        }
    }

    for (const auto& old_name : handle->output_names)
    {
        if (next_names.count(old_name))
        {
            continue;
        }

        auto *output = wf::get_core().output_layout->find_output(old_name);
        if (output && output->handle)
        {
            if (auto *output_resource = find_output_resource_for_client(output->handle, get_client()))
            {
                ext_workspace_group_handle_v1_send_output_leave(handle->resource, output_resource);
            }
        }
    }

    handle->output_names = std::move(next_names);
}

void manager_client_t::sync_group_workspaces(group_handle_t *handle, const std::set<workspace_key_t>& keys)
{
    if (!handle || !handle->resource)
    {
        return;
    }

    for (const auto& key : keys)
    {
        auto workspace = workspaces.find(key);
        if ((workspace != workspaces.end()) && !handle->workspaces.count(key) && workspace->second->resource)
        {
            ext_workspace_group_handle_v1_send_workspace_enter(handle->resource, workspace->second->resource);
        }
    }

    for (const auto& key : handle->workspaces)
    {
        if (keys.count(key))
        {
            continue;
        }

        auto workspace = workspaces.find(key);
        if ((workspace != workspaces.end()) && workspace->second->resource)
        {
            ext_workspace_group_handle_v1_send_workspace_leave(handle->resource, workspace->second->resource);
        }
    }

    handle->workspaces = keys;
}

void manager_client_t::sync()
{
    if (!resource || stopped)
    {
        return;
    }

    auto snapshot = get_current_snapshot();

    for (auto& [wset_index, current] : snapshot.groups)
    {
        ensure_group(wset_index);
    }

    for (auto& [key, wset] : snapshot.workspaces)
    {
        auto *workspace = ensure_workspace(key);
        send_workspace_details(workspace, wset);
    }

    for (auto& [wset_index, current] : snapshot.groups)
    {
        auto *group = ensure_group(wset_index);
        sync_group_outputs(group, current.outputs);

        std::set<workspace_key_t> keys;
        for (const auto& [key, wset] : snapshot.workspaces)
        {
            if (key.wset == wset_index)
            {
                keys.insert(key);
            }
        }

        sync_group_workspaces(group, keys);
    }

    auto workspace_it = workspaces.begin();
    while (workspace_it != workspaces.end())
    {
        if (snapshot.workspaces.count(workspace_it->first))
        {
            ++workspace_it;
            continue;
        }

        auto removed = std::move(workspace_it->second);
        workspaces.erase(workspace_it++);

        for (auto& [_, group] : groups)
        {
            if (group->workspaces.erase(removed->key) && group->resource && removed->resource)
            {
                ext_workspace_group_handle_v1_send_workspace_leave(group->resource, removed->resource);
            }
        }

        removed->removed = true;
        if (removed->resource)
        {
            ext_workspace_handle_v1_send_removed(removed->resource);
            retired_workspaces.push_back(std::move(removed));
        }
    }

    auto group_it = groups.begin();
    while (group_it != groups.end())
    {
        if (snapshot.groups.count(group_it->first))
        {
            ++group_it;
            continue;
        }

        auto removed = std::move(group_it->second);
        groups.erase(group_it++);
        removed->removed = true;
        if (removed->resource)
        {
            ext_workspace_group_handle_v1_send_removed(removed->resource);
            retired_groups.push_back(std::move(removed));
        }
    }

    send_done();
}

void manager_client_t::commit()
{
    if (stopped)
    {
        return;
    }

    auto snapshot = get_current_snapshot();

    for (const auto& key : pending_removal)
    {
        auto workspace = snapshot.workspaces.find(key);
        if (workspace == snapshot.workspaces.end() || !workspace->second)
        {
            continue;
        }

        auto grid = workspace->second->get_workspace_grid_size();
        wf::dimensions_t next_grid = grid;
        if ((key.x == grid.width - 1) && (grid.width > 1))
        {
            next_grid.width--;
        } else if ((key.y == grid.height - 1) && (grid.height > 1))
        {
            next_grid.height--;
        } else
        {
            continue;
        }

        workspace->second->set_workspace_grid_size(next_grid);
        plugin->forget_workspace_names(key.wset, next_grid);
    }

    for (const auto& [wset_index, name] : pending_creation)
    {
        auto group = snapshot.groups.find(wset_index);
        if ((group == snapshot.groups.end()) || !group->second.wset)
        {
            continue;
        }

        auto grid = group->second.wset->get_workspace_grid_size();
        workspace_key_t key{wset_index, grid.width, 0};
        plugin->set_workspace_name(key, name);
        group->second.wset->set_workspace_grid_size({grid.width + 1, grid.height});
    }

    for (const auto& [wset_index, workspace] : pending_activation)
    {
        auto group = snapshot.groups.find(wset_index);
        if ((group != snapshot.groups.end()) && group->second.wset &&
            group->second.wset->is_workspace_valid(workspace))
        {
            group->second.wset->request_workspace(workspace);
        }
    }

    pending_activation.clear();
    pending_creation.clear();
    pending_removal.clear();
    plugin->broadcast_sync();
}

void manager_client_t::finish()
{
    if (!resource || stopped)
    {
        return;
    }

    stopped = true;
    ext_workspace_manager_v1_send_finished(resource);
    wl_resource_destroy(resource);
}

void manager_client_t::forget_workspace(workspace_handle_t *handle)
{
    if (!handle)
    {
        return;
    }

    auto active = workspaces.find(handle->key);
    if ((active != workspaces.end()) && (active->second.get() == handle))
    {
        workspaces.erase(active);
        return;
    }

    auto it = std::remove_if(retired_workspaces.begin(), retired_workspaces.end(),
        [=] (const auto& item) { return item.get() == handle; });
    retired_workspaces.erase(it, retired_workspaces.end());
}

void manager_client_t::forget_group(group_handle_t *handle)
{
    if (!handle)
    {
        return;
    }

    auto active = groups.find(handle->wset_index);
    if ((active != groups.end()) && (active->second.get() == handle))
    {
        groups.erase(active);
        return;
    }

    auto it = std::remove_if(retired_groups.begin(), retired_groups.end(),
        [=] (const auto& item) { return item.get() == handle; });
    retired_groups.erase(it, retired_groups.end());
}
}

DECLARE_WAYFIRE_PLUGIN(ext_workspace_manager_plugin_t);
