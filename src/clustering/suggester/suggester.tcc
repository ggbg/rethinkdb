#ifndef __CLUSTERING_SUGGESTER_SUGGESTER_TCC__
#define __CLUSTERING_SUGGESTER_SUGGESTER_TCC__

/* Returns a "score" indicating how expensive it would be to turn the machine
with the given business card into a primary or secondary for the given shard. */

template<class protocol_t>
float estimate_cost_to_get_up_to_date(
        const reactor_business_card_t<protocol_t> &business_card,
        const typename protocol_t::region_t &shard) {
    typedef reactor_business_card_t<protocol_t> rb_t;
    region_map_t<protocol_t, float> costs(shard, 3);
    for (typename rb_t::activity_map_t::const_iterator it = business_card.activities.begin();
            it != business_card.activities.end(); it++) {
        typename protocol_t::region_t intersection = region_intersection(it->second.first, shard);
        if (!region_is_empty(intersection)) {
            int cost;
            if (boost::get<typename rb_t::primary_when_safe_t>(&it->second.second)) {
                cost = 0;
            } else if (boost::get<typename rb_t::primary_t>(&it->second.second)) {
                cost = 0;
            } else if (boost::get<typename rb_t::secondary_up_to_date_t>(&it->second.second)) {
                cost = 1;
            } else if (boost::get<typename rb_t::secondary_without_primary_t>(&it->second.second)) {
                cost = 2;
            } else if (boost::get<typename rb_t::secondary_backfilling_t>(&it->second.second)) {
                cost = 2;
            } else if (boost::get<typename rb_t::nothing_when_safe_t>(&it->second.second)) {
                cost = 3;
            } else if (boost::get<typename rb_t::nothing_when_done_erasing_t>(&it->second.second)) {
                cost = 3;
            } else if (boost::get<typename rb_t::nothing_t>(&it->second.second)) {
                cost = 3;
            }
            /* It's ok to just call `set()` instead of trying to find the minimum
            because activities should never overlap. */
            costs.set(intersection, cost);
        }
    }
    float sum = 0;
    int count = 0;
    for (typename region_map_t<protocol_t, float>::iterator it = costs.begin(); it != costs.end(); it++) {
        /* TODO: Scale by how much data is in `it->first` */
        sum += it->second;
        count++;
    }
    return sum / count;
}

inline std::vector<peer_id_t> pick_n_best(const std::multimap<float, peer_id_t> candidates, int n) {
    std::vector<peer_id_t> result;
    std::multimap<float, peer_id_t>::const_iterator it = candidates.begin();
    while ((int)result.size() < n) {
        if (it == candidates.end()) {
            throw cannot_satisfy_goals_exc_t();
        }
        float block_value = it->first;
        std::vector<peer_id_t> block;
        while (it != candidates.upper_bound(block_value)) {
            block.push_back(it->second);
            it++;
        }
        std::random_shuffle(block.begin(), block.end());
        int i = 0;
        while ((int)result.size() < n && i < (int)block.size()) {
            result.push_back(block[i]);
            i++;
        }
    }
    return result;
}

template<class protocol_t>
std::map<peer_id_t, typename blueprint_details::role_t> suggest_blueprint_for_shard(
        const std::map<peer_id_t, reactor_business_card_t<protocol_t> > &directory,
        const datacenter_id_t &primary_datacenter,
        const std::map<datacenter_id_t, int> &datacenter_affinities,
        const typename protocol_t::region_t &shard,
        const std::map<peer_id_t, datacenter_id_t> &machine_data_centers) {

    std::map<peer_id_t, typename blueprint_details::role_t> sub_blueprint;

    std::multimap<float, peer_id_t> primary_candidates;
    for (std::map<peer_id_t, datacenter_id_t>::const_iterator it = machine_data_centers.begin();
            it != machine_data_centers.end(); it++) {
        if (it->second == primary_datacenter) {
            float cost = estimate_cost_to_get_up_to_date(directory.find(it->first)->second, shard);
            primary_candidates.insert(std::make_pair(cost, it->first));
        }
    }
    peer_id_t primary = pick_n_best(primary_candidates, 1).front();
    sub_blueprint[primary] = blueprint_details::role_primary;

    for (std::map<datacenter_id_t, int>::const_iterator it = datacenter_affinities.begin(); it != datacenter_affinities.end(); it++) {
        std::multimap<float, peer_id_t> secondary_candidates;
        for (std::map<peer_id_t, datacenter_id_t>::const_iterator jt = machine_data_centers.begin();
                jt != machine_data_centers.end(); jt++) {
            if (jt->second == it->first && jt->first != primary) {
                float cost = estimate_cost_to_get_up_to_date(directory.find(jt->first)->second, shard);
                secondary_candidates.insert(std::make_pair(cost, jt->first));
            }
        }
        std::vector<peer_id_t> secondaries = pick_n_best(secondary_candidates, it->second);
        for (std::vector<peer_id_t>::iterator jt = secondaries.begin(); jt != secondaries.end(); jt++) {
            sub_blueprint[*jt] = blueprint_details::role_secondary;
        }
    }

    for (std::map<peer_id_t, datacenter_id_t>::const_iterator it = machine_data_centers.begin();
            it != machine_data_centers.end(); it++) {
        /* This will set the value to `role_nothing` iff the peer is not already
        in the blueprint. */
        sub_blueprint.insert(std::make_pair(it->first, blueprint_details::role_nothing));
    }

    return sub_blueprint;
}

template<class protocol_t>
blueprint_t<protocol_t> suggest_blueprint(
        const std::map<peer_id_t, reactor_business_card_t<protocol_t> > &directory,
        const datacenter_id_t &primary_datacenter,
        const std::map<datacenter_id_t, int> &datacenter_affinities,
        const std::set<typename protocol_t::region_t> &shards,
        const std::map<peer_id_t, datacenter_id_t> &machine_data_centers) {

    blueprint_t<protocol_t> blueprint;
    for (typename std::set<typename protocol_t::region_t>::const_iterator it = shards.begin();
            it != shards.end(); it++) {
        std::map<peer_id_t, typename blueprint_details::role_t> shard_blueprint =
            suggest_blueprint_for_shard(directory, primary_datacenter, datacenter_affinities, *it, machine_data_centers);
        for (typename std::map<peer_id_t, typename blueprint_details::role_t>::iterator jt = shard_blueprint.begin();
                jt != shard_blueprint.end(); jt++) {
            blueprint.peers_roles[jt->first][*it] = jt->second;
        }
    }
    return blueprint;
}

#endif /* __CLUSTERING_SUGGESTER_SUGGESTER_TCC__ */
