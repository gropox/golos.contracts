#include "golos.publication.hpp"
#include <common/hash64.hpp>
#include <eosiolib/transaction.hpp>
#include <eosio.token/eosio.token.hpp>
#include <golos.social/golos.social.hpp>
#include <golos.vesting/golos.vesting.hpp>
#include <golos.charge/golos.charge.hpp>
#include <common/upsert.hpp>
#include "utils.hpp"

namespace golos {

using namespace atmsp::storable;

extern "C" {
    void apply(uint64_t receiver, uint64_t code, uint64_t action) {
        //publication(receiver).apply(code, action);
        auto execute_action = [&](const auto fn) {
            return eosio::execute_action(eosio::name(receiver), eosio::name(code), fn);
        };

#define NN(x) N(x).value

        if (NN(transfer) == action && config::token_name.value == code)
            execute_action(&publication::on_transfer);

        if (NN(createmssg) == action)
            execute_action(&publication::create_message);
        if (NN(updatemssg) == action)
            execute_action(&publication::update_message);
        if (NN(deletemssg) == action)
            execute_action(&publication::delete_message);
        if (NN(upvote) == action)
            execute_action(&publication::upvote);
        if (NN(downvote) == action)
            execute_action(&publication::downvote);
        if (NN(unvote) == action)
            execute_action(&publication::unvote);
        if (NN(closemssg) == action)
            execute_action(&publication::close_message);
        if (NN(setrules) == action)
            execute_action(&publication::set_rules);
        if (NN(setlimit) == action)
            execute_action(&publication::set_limit);
        if (NN(setparams) == action)
            execute_action(&publication::set_params);
        if (NN(reblog) == action)
            execute_action(&publication::reblog);
    }
#undef NN
}

struct posting_params_setter: set_params_visitor<posting_state> {
    using set_params_visitor::set_params_visitor;

    bool operator()(const max_vote_changes_prm& param) {
        return set_param(param, &posting_state::max_vote_changes_param);
    }

    bool operator()(const cashout_window_prm& param) {
        return set_param(param, &posting_state::cashout_window_param);
    }

    bool operator()(const max_beneficiaries_prm& param) {
        return set_param(param, &posting_state::max_beneficiaries_param);
    }

    bool operator()(const max_comment_depth_prm& param) {
        return set_param(param, &posting_state::max_comment_depth_param);
    }
    
    bool operator()(const social_acc_prm& param) {
        return set_param(param, &posting_state::social_acc_param);
    }
    
    bool operator()(const referral_acc_prm& param) {
        return set_param(param, &posting_state::referral_acc_param);
    }
};

void publication::create_message(name account, std::string permlink,
                              name parentacc, std::string parentprmlnk,
                              std::vector<structures::beneficiary> beneficiaries,
                            //actually, beneficiaries[i].prop _here_ should be interpreted as (share * _100percent)
                            //but not as a raw data for elaf_t, may be it's better to use another type (with uint16_t field for prop).
                              int64_t tokenprop, bool vestpayment,
                              std::string headermssg,
                              std::string bodymssg, std::string languagemssg,
                              std::vector<structures::tag> tags,
                              std::string jsonmetadata) {
    require_auth(account);

    posting_params_singleton cfg(_self, _self.value);
    const auto &cashout_window_param = cfg.get().cashout_window_param;
    const auto &max_beneficiaries_param = cfg.get().max_beneficiaries_param;
    const auto &max_comment_depth_param = cfg.get().max_comment_depth_param;
    const auto &social_acc_param = cfg.get().social_acc_param;

    if (parentacc) {
        if (social_acc_param.account) {
            eosio_assert(!social::is_blocking(social_acc_param.account, parentacc, account), "You are blocked by this account");
        }
    }

    tables::reward_pools pools(_self, _self.value);
    auto pool = pools.begin();   // TODO: Reverse iterators doesn't work correctly
    eosio_assert(pool != pools.end(), "publication::create_message: [pools] is empty");
    pool = --pools.end();
    check_account(account, pool->state.funds.symbol);
    auto token_code = pool->state.funds.symbol.code();
    auto issuer = eosio::token::get_issuer(config::token_name, token_code);
    tables::limit_table lims(_self, _self.value);
    
    use_charge(lims, parentacc ? structures::limitparams::COMM : structures::limitparams::POST, issuer, account, 
        golos::vesting::get_account_effective_vesting(config::vesting_name, account, token_code).amount, token_code, vestpayment);
            
    auto message_id = hash64(permlink);
    if(!parentacc)
        use_postbw_charge(lims, issuer, account, token_code, message_id);

    std::map<name, int64_t> benefic_map;
    int64_t prop_sum = 0;
    for (auto& ben : beneficiaries) {
        check_account(ben.account, pool->state.funds.symbol);
        eosio_assert(0 < ben.deductprcnt && ben.deductprcnt <= config::_100percent, "publication::create_message: wrong ben.prop value");
        prop_sum += ben.deductprcnt;
        eosio_assert(prop_sum <= config::_100percent, "publication::create_message: prop_sum > 100%");
        benefic_map[ben.account] += ben.deductprcnt; //several entries for one user? ok.
    }
    eosio_assert((benefic_map.size() <= max_beneficiaries_param.max_beneficiaries), "publication::create_message: benafic_map.size() > MAX_BENEFICIARIES");

    //reusing a vector
    beneficiaries.reserve(benefic_map.size());
    beneficiaries.clear();
    for(auto & ben : benefic_map)
        beneficiaries.emplace_back(structures::beneficiary{
            .account = ben.first,
            .deductprcnt = static_cast<base_t>(get_limit_prop(ben.second).data())
        });

    auto cur_time = current_time();
    atmsp::machine<fixp_t> machine;

    eosio_assert(cur_time >= pool->created, "publication::create_message: cur_time < pool.created");
    eosio_assert(pool->state.msgs < std::numeric_limits<structures::counter_t>::max(), "publication::create_message: pool->msgs == max_counter_val");
    pools.modify(*pool, _self, [&](auto &item){ item.state.msgs++; });

    tables::message_table message_table(_self, account.value);
    eosio_assert(message_table.find(message_id) == message_table.end(), "This message already exists.");

    tables::content_table content_table(_self, account.value);
    auto parent_id = parentacc ? hash64(parentprmlnk) : 0;

    uint8_t level = 0;
    if(parentacc)
        level = 1 + notify_parent(true, parentacc, parent_id);
    eosio_assert(level <= max_comment_depth_param.max_comment_depth, "publication::create_message: level > MAX_COMMENT_DEPTH");

    auto mssg_itr = message_table.emplace(account, [&]( auto &item ) {
        item.id = message_id;
        item.permlink = permlink;
        item.date = cur_time;
        item.parentacc = parentacc;
        item.parent_id = parent_id;
        item.tokenprop = static_cast<base_t>(std::min(get_limit_prop(tokenprop), ELF(pool->rules.maxtokenprop)).data()),
        item.beneficiaries = beneficiaries;
        item.rewardweight = static_cast<base_t>(elaf_t(1).data()); //we will get actual value from charge on post closing
        item.childcount = 0;
        item.closed = false;
        item.level = level;
    });
    content_table.emplace(account, [&]( auto &item ) {
        item.id = message_id;
        item.headermssg = headermssg;
        item.bodymssg = bodymssg;
        item.languagemssg = languagemssg;
        item.tags = tags;
        item.jsonmetadata = jsonmetadata;
    });

    structures::accandvalue parent {parentacc, parent_id};
    uint64_t seconds_diff = 0;
    bool closed = false;
    while (parent.account) {
        tables::message_table parent_message_table(_self, parent.account.value);
        auto parent_itr = parent_message_table.find(parent.value);
        eosio_assert(cur_time >= parent_itr->date, "publication::create_message: cur_time < parent_itr->date");
        seconds_diff = cur_time - parent_itr->date;
        parent.account = parent_itr->parentacc;
        parent.value = parent_itr->parent_id;
        closed = parent_itr->closed;
    }
    seconds_diff /= eosio::seconds(1).count();
    uint64_t delay_sec = cashout_window_param.window > seconds_diff ? cashout_window_param.window - seconds_diff : 0;
    if (!closed && delay_sec)
        close_message_timer(account, message_id, delay_sec);
    else //parent is already closed or is about to
        message_table.modify(mssg_itr, _self, [&]( auto &item) { item.closed = true; });
}

void publication::update_message(name account, std::string permlink,
                              std::string headermssg, std::string bodymssg,
                              std::string languagemssg, std::vector<structures::tag> tags,
                              std::string jsonmetadata) {
    require_auth(account);
    tables::content_table content_table(_self, account.value);
    auto cont_itr = content_table.find(hash64(permlink));
    eosio_assert(cont_itr != content_table.end(), "Content doesn't exist.");

    content_table.modify(cont_itr, account, [&]( auto &item ) {
        item.headermssg = headermssg;
        item.bodymssg = bodymssg;
        item.languagemssg = languagemssg;
        item.tags = tags;
        item.jsonmetadata = jsonmetadata;
    });
}

auto publication::get_pool(tables::reward_pools& pools, uint64_t time) {
    eosio_assert(pools.begin() != pools.end(), "publication::get_pool: [pools] is empty");

    auto pool = pools.upper_bound(time);

    eosio_assert(pool != pools.begin(), "publication::get_pool: can't find an appropriate pool");
    return (--pool);
}

void publication::delete_message(name account, std::string permlink) {
    require_auth(account);

    tables::message_table message_table(_self, account.value);
    tables::content_table content_table(_self, account.value);
    tables::vote_table vote_table(_self, account.value);

    auto message_id = hash64(permlink);
    auto mssg_itr = message_table.find(message_id);
    eosio_assert(mssg_itr != message_table.end(), "Message doesn't exist.");
    eosio_assert((mssg_itr->childcount) == 0, "You can't delete comment with child comments.");
    eosio_assert(FP(mssg_itr->state.netshares) <= 0, "Cannot delete a comment with net positive votes.");
    auto cont_itr = content_table.find(message_id);
    eosio_assert(cont_itr != content_table.end(), "Content doesn't exist.");

    if(mssg_itr->parentacc)
        notify_parent(false, mssg_itr->parentacc, mssg_itr->parent_id);
    else {
        tables::reward_pools pools(_self, _self.value);
        remove_postbw_charge(account, get_pool(pools, mssg_itr->date)->state.funds.symbol.code(), mssg_itr->id);
    }

    message_table.erase(mssg_itr);
    content_table.erase(cont_itr);

    auto votetable_index = vote_table.get_index<"messageid"_n>();
    auto vote_itr = votetable_index.lower_bound(message_id);
    while ((vote_itr != votetable_index.end()) && (vote_itr->message_id == message_id))
        vote_itr = votetable_index.erase(vote_itr);
}

void publication::upvote(name voter, name author, string permlink, uint16_t weight) {
    eosio_assert(weight > 0, "weight can't be 0.");
    eosio_assert(weight <= config::_100percent, "weight can't be more than 100%.");
    set_vote(voter, author, permlink, weight);
}

void publication::downvote(name voter, name author, string permlink, uint16_t weight) {
    eosio_assert(weight > 0, "weight can't be 0.");
    eosio_assert(weight <= config::_100percent, "weight can't be more than 100%.");
    set_vote(voter, author, permlink, -weight);
}

void publication::unvote(name voter, name author, string permlink) {
    set_vote(voter, author, permlink, 0);
}

void publication::payto(name user, eosio::asset quantity, enum_t mode) {
    require_auth(_self);
    eosio_assert(quantity.amount >= 0, "LOGIC ERROR! publication::payto: quantity.amount < 0");
    if(quantity.amount == 0)
        return;

    if(static_cast<payment_t>(mode) == payment_t::TOKEN)
        INLINE_ACTION_SENDER(eosio::token, transfer) (config::token_name, {_self, config::active_name}, {_self, user, quantity, ""});
    else if(static_cast<payment_t>(mode) == payment_t::VESTING)
        INLINE_ACTION_SENDER(eosio::token, transfer) (config::token_name, {_self, config::active_name},
            {_self, config::vesting_name, quantity, config::send_prefix + name{user}.to_string()});
    else
        eosio_assert(false, "publication::payto: unknown kind of payment");
}

int64_t publication::pay_curators(name author, uint64_t msgid, int64_t max_rewards, fixp_t weights_sum, eosio::symbol tokensymbol) {
    tables::vote_table vs(_self, author.value);
    int64_t unclaimed_rewards = max_rewards;

    auto idx = vs.get_index<"messageid"_n>();
    auto v = idx.lower_bound(msgid);
    while ((v != idx.end()) && (v->message_id == msgid)) {
        if((weights_sum > fixp_t(0)) && (max_rewards > 0)) {
            auto claim = int_cast(elai_t(max_rewards) * elaf_t(FP(v->curatorsw) / weights_sum));
            eosio_assert(claim <= unclaimed_rewards, "LOGIC ERROR! publication::pay_curators: claim > unclaimed_rewards");
            if(claim > 0) {
                unclaimed_rewards -= claim;
                payto(v->voter, eosio::asset(claim, tokensymbol), static_cast<enum_t>(payment_t::VESTING));
            }
        }
        //v = idx.erase(v);
        ++v;
    }
    return unclaimed_rewards;
}

void publication::remove_postbw_charge(name account, symbol_code token_code, int64_t mssg_id, elaf_t* reward_weight_ptr) {
    auto issuer = eosio::token::get_issuer(config::token_name, token_code);
    tables::limit_table lims(_self, _self.value);
    auto bw_lim_itr = lims.find(structures::limitparams::POSTBW);
    eosio_assert(bw_lim_itr != lims.end(), "publication::remove_postbw_charge: limit parameters not set");
    auto post_charge = golos::charge::get_stored(config::charge_name, account, token_code, bw_lim_itr->charge_id, mssg_id);
    if(post_charge >= 0)
        INLINE_ACTION_SENDER(charge, removestored) (config::charge_name, 
            {issuer, config::invoice_name}, {
                account, 
                token_code, 
                bw_lim_itr->charge_id,
                mssg_id
            });
    if(reward_weight_ptr)
        *reward_weight_ptr = (post_charge > bw_lim_itr->cutoff) ? 
            static_cast<elaf_t>(elai_t(bw_lim_itr->cutoff) / elai_t(post_charge)) : elaf_t(1);
}
void publication::use_charge(tables::limit_table& lims, structures::limitparams::act_t act, name issuer,
                            name account, int64_t eff_vesting, symbol_code token_code, bool vestpayment, elaf_t weight) {
    auto lim_itr = lims.find(act);
    eosio_assert(lim_itr != lims.end(), "publication::use_charge: limit parameters not set");
    eosio_assert(eff_vesting >= lim_itr->min_vesting, "insufficient effective vesting amount");
    if(lim_itr->price >= 0)
        INLINE_ACTION_SENDER(charge, use) (config::charge_name, 
            {issuer, config::invoice_name}, {
                account, 
                token_code, 
                lim_itr->charge_id, 
                int_cast(elai_t(lim_itr->price) * weight), 
                lim_itr->cutoff,
                vestpayment ? int_cast(elai_t(lim_itr->vesting_price) * weight) : 0
            });
}

void publication::use_postbw_charge(tables::limit_table& lims, name issuer, name account, symbol_code token_code, int64_t mssg_id) {
    auto bw_lim_itr = lims.find(structures::limitparams::POSTBW);
    if(bw_lim_itr->price >= 0)
        INLINE_ACTION_SENDER(charge, useandstore) (config::charge_name, 
            {issuer, config::invoice_name}, {
                account, 
                token_code, 
                bw_lim_itr->charge_id,
                mssg_id,
                bw_lim_itr->price
            });
}

void publication::close_message(name account, uint64_t id) {
    require_auth(_self);
    tables::message_table message_table(_self, account.value);
    auto mssg_itr = message_table.find(id);
    eosio_assert(mssg_itr != message_table.end(), "Message doesn't exist.");
    eosio_assert(!mssg_itr->closed, "Message is already closed.");

    tables::reward_pools pools(_self, _self.value);
    auto pool = get_pool(pools, mssg_itr->date);

    eosio_assert(pool->state.msgs != 0, "LOGIC ERROR! publication::payrewards: pool.msgs is equal to zero");
    atmsp::machine<fixp_t> machine;
    fixp_t sharesfn = set_and_run(machine, pool->rules.mainfunc.code, {FP(mssg_itr->state.netshares)}, {{fixp_t(0), FP(pool->rules.mainfunc.maxarg)}});

    auto state = pool->state;
    auto reward_weight = elaf_t(1);
    int64_t payout = 0;
    if(state.msgs == 1) {
        payout = state.funds.amount;
        eosio_assert(state.rshares == mssg_itr->state.netshares, "LOGIC ERROR! publication::payrewards: pool->rshares != mssg_itr->netshares for last message");
        eosio_assert(state.rsharesfn == sharesfn.data(), "LOGIC ERROR! publication::payrewards: pool->rsharesfn != sharesfn.data() for last message");
        state.funds.amount = 0;
        state.rshares = 0;
        state.rsharesfn = 0;
    }
    else {
        auto total_rsharesfn = WP(state.rsharesfn);

        if(sharesfn > fixp_t(0)) {
            eosio_assert(total_rsharesfn > 0, "LOGIC ERROR! publication::payrewards: total_rshares_fn <= 0");

            auto numer = sharesfn;
            auto denom = total_rsharesfn;
            narrow_down(numer, denom);
            
            if(!mssg_itr->parentacc)
                remove_postbw_charge(account, pool->state.funds.symbol.code(), mssg_itr->id, &reward_weight);

            payout = int_cast(reward_weight * elai_t(elai_t(state.funds.amount) * static_cast<elaf_t>(elap_t(numer) / elap_t(denom))));
            state.funds.amount -= payout;
            eosio_assert(state.funds.amount >= 0, "LOGIC ERROR! publication::payrewards: state.funds < 0");
        }

        auto new_rshares = WP(state.rshares) - wdfp_t(FP(mssg_itr->state.netshares));
        auto new_rsharesfn = WP(state.rsharesfn) - wdfp_t(sharesfn);

        eosio_assert(new_rshares >= 0, "LOGIC ERROR! publication::payrewards: new_rshares < 0");
        eosio_assert(new_rsharesfn >= 0, "LOGIC ERROR! publication::payrewards: new_rsharesfn < 0");

        state.rshares = new_rshares.data();
        state.rsharesfn = new_rsharesfn.data();
    }

    auto curation_payout = int_cast(ELF(pool->rules.curatorsprop) * elai_t(payout));

    eosio_assert((curation_payout <= payout) && (curation_payout >= 0), "publication::payrewards: wrong curation_payout");

    auto unclaimed_rewards = pay_curators(account, id, curation_payout, FP(mssg_itr->state.sumcuratorsw), state.funds.symbol);

    eosio_assert(unclaimed_rewards >= 0, "publication::payrewards: unclaimed_rewards < 0");

    state.funds.amount += unclaimed_rewards;
    payout -= curation_payout;

    int64_t ben_payout_sum = 0;
    for(auto& ben : mssg_itr->beneficiaries) {
        auto ben_payout = int_cast(elai_t(payout) * ELF(ben.deductprcnt));
        eosio_assert((0 <= ben_payout) && (ben_payout <= payout - ben_payout_sum), "LOGIC ERROR! publication::payrewards: wrong ben_payout value");
        payto(ben.account, eosio::asset(ben_payout, state.funds.symbol), static_cast<enum_t>(payment_t::VESTING));
        ben_payout_sum += ben_payout;
    }
    payout -= ben_payout_sum;

    auto token_payout = int_cast(elai_t(payout) * ELF(mssg_itr->tokenprop));
    eosio_assert(payout >= token_payout, "publication::payrewards: wrong token_payout value");

    payto(account, eosio::asset(token_payout, state.funds.symbol), static_cast<enum_t>(payment_t::TOKEN));
    payto(account, eosio::asset(payout - token_payout, state.funds.symbol), static_cast<enum_t>(payment_t::VESTING));

    //message_table.erase(mssg_itr);
    message_table.modify(mssg_itr, _self, [&]( auto &item) {
        item.rewardweight = static_cast<base_t>(reward_weight.data()); //not used to calculate rewards, stored for stats only
        item.closed = true;
    });

    bool pool_erased = false;
    state.msgs--;
    if(state.msgs == 0) {
        if(pool != --pools.end()) {//there is a pool after, so we can delete this one
            eosio_assert(state.funds.amount == unclaimed_rewards, "LOGIC ERROR! publication::payrewards: state.funds != unclaimed_rewards");
            fill_depleted_pool(pools, state.funds, pool);
            pools.erase(pool);
            pool_erased = true;
        }
    }
    if(!pool_erased)
        pools.modify(pool, _self, [&](auto &item) { item.state = state; });
}

void publication::close_message_timer(name account, uint64_t id, uint64_t delay_sec) {
    transaction trx;
    trx.actions.emplace_back(action{permission_level(_self, config::active_name), _self, "closemssg"_n, structures::accandvalue{account, id}});
    trx.delay_sec = delay_sec;
    trx.send((static_cast<uint128_t>(id) << 64) | account.value, _self);
}

void publication::check_upvote_time(uint64_t cur_time, uint64_t mssg_date) {
    posting_params_singleton cfg(_self, _self.value);
    const auto &cashout_window_param = cfg.get().cashout_window_param;

    eosio_assert((cur_time <= mssg_date + ((cashout_window_param.window - cashout_window_param.upvote_lockout) * seconds(1).count())) ||
                 (cur_time > mssg_date + (cashout_window_param.window * seconds(1).count())),
                  "You can't upvote, because publication will be closed soon.");
}

fixp_t publication::calc_available_rshares(name voter, int16_t weight, uint64_t cur_time, const structures::rewardpool& pool) {
    elaf_t abs_w = get_limit_prop(abs(weight));
    tables::limit_table lims(_self, _self.value);
    auto token_code = pool.state.funds.symbol.code();
    int64_t eff_vesting = golos::vesting::get_account_effective_vesting(config::vesting_name, voter, token_code).amount;
    use_charge(lims, structures::limitparams::VOTE, eosio::token::get_issuer(config::token_name, token_code),
        voter, eff_vesting, token_code, false, abs_w);
    fixp_t abs_rshares = fp_cast<fixp_t>(eff_vesting, false) * abs_w;
    return (weight < 0) ? -abs_rshares : abs_rshares;
}

void publication::set_vote(name voter, name author, string permlink, int16_t weight) {
    require_auth(voter);

    posting_params_singleton cfg(_self, _self.value);
    const auto &max_vote_changes_param = cfg.get().max_vote_changes_param;
    const auto &social_acc_param = cfg.get().social_acc_param;

    uint64_t id = hash64(permlink);
    tables::message_table message_table(_self, author.value);
    auto mssg_itr = message_table.find(id);
    eosio_assert(mssg_itr != message_table.end(), "Message doesn't exist.");
    tables::reward_pools pools(_self, _self.value);
    auto pool = get_pool(pools, mssg_itr->date);
    check_account(voter, pool->state.funds.symbol);
    tables::vote_table vote_table(_self, author.value);

    auto cur_time = current_time();

    auto votetable_index = vote_table.get_index<"messageid"_n>();
    auto vote_itr = votetable_index.lower_bound(id);
    while ((vote_itr != votetable_index.end()) && (vote_itr->message_id == id)) {
        if (voter == vote_itr->voter) {
            // it's not consensus part and can be moved to storage in future
            if (mssg_itr->closed) {
                votetable_index.modify(vote_itr, voter, [&]( auto &item ) {
                    item.count = -1;
                });
                return;
            }

            eosio_assert(weight != vote_itr->weight, "Vote with the same weight has already existed.");
            eosio_assert(vote_itr->count != max_vote_changes_param.max_vote_changes, "You can't revote anymore.");

            atmsp::machine<fixp_t> machine;
            fixp_t rshares = calc_available_rshares(voter, weight, cur_time, *pool);
            if(rshares > FP(vote_itr->rshares))
                check_upvote_time(cur_time, mssg_itr->date);

            fixp_t new_mssg_rshares = (FP(mssg_itr->state.netshares) - FP(vote_itr->rshares)) + rshares;
            auto rsharesfn_delta = get_delta(machine, FP(mssg_itr->state.netshares), new_mssg_rshares, pool->rules.mainfunc);

            pools.modify(*pool, _self, [&](auto &item) {
                item.state.rshares = ((WP(item.state.rshares) - wdfp_t(FP(vote_itr->rshares))) + wdfp_t(rshares)).data();
                item.state.rsharesfn = (WP(item.state.rsharesfn) + wdfp_t(rsharesfn_delta)).data();
            });

            message_table.modify(mssg_itr, name(), [&]( auto &item ) {
                item.state.netshares = new_mssg_rshares.data();
                item.state.sumcuratorsw = (FP(item.state.sumcuratorsw) - FP(vote_itr->curatorsw)).data();
            });

            votetable_index.modify(vote_itr, voter, [&]( auto &item ) {
               item.weight = weight;
               item.time = cur_time;
               item.curatorsw = fixp_t(0).data();
               item.rshares = rshares.data();
               ++item.count;
            });

            return;
        }
        ++vote_itr;
    }
    atmsp::machine<fixp_t> machine;
    fixp_t rshares = calc_available_rshares(voter, weight, cur_time, *pool);
    if(rshares > 0)
        check_upvote_time(cur_time, mssg_itr->date);

    structures::messagestate msg_new_state = {
        .netshares = add_cut(FP(mssg_itr->state.netshares), rshares).data(),
        .voteshares = ((rshares > fixp_t(0)) ?
            add_cut(FP(mssg_itr->state.voteshares), rshares) :
            FP(mssg_itr->state.voteshares)).data()
        //.sumcuratorsw = see below
    };

    auto rsharesfn_delta = get_delta(machine, FP(mssg_itr->state.netshares), FP(msg_new_state.netshares), pool->rules.mainfunc);

    pools.modify(*pool, _self, [&](auto &item) {
         item.state.rshares = (WP(item.state.rshares) + wdfp_t(rshares)).data();
         item.state.rsharesfn = (WP(item.state.rsharesfn) + wdfp_t(rsharesfn_delta)).data();
    });
    eosio_assert(WP(pool->state.rshares) >= 0, "pool state rshares overflow");
    eosio_assert(WP(pool->state.rsharesfn) >= 0, "pool state rsharesfn overflow");

    auto sumcuratorsw_delta = get_delta(machine, FP(mssg_itr->state.voteshares), FP(msg_new_state.voteshares), pool->rules.curationfunc);
    msg_new_state.sumcuratorsw = (FP(mssg_itr->state.sumcuratorsw) + sumcuratorsw_delta).data();
    message_table.modify(mssg_itr, _self, [&](auto &item) { item.state = msg_new_state; });

    auto time_delta = static_cast<int64_t>((cur_time - mssg_itr->date) / seconds(1).count());
    auto curatorsw_factor =
        std::max(std::min(
        set_and_run(machine, pool->rules.timepenalty.code, {fp_cast<fixp_t>(time_delta, false)}, {{fixp_t(0), FP(pool->rules.timepenalty.maxarg)}}),
        fixp_t(1)), fixp_t(0));

    vote_table.emplace(voter, [&]( auto &item ) {
        item.id = vote_table.available_primary_key();
        item.message_id = mssg_itr->id;
        item.voter = voter;
        item.weight = weight;
        item.time = cur_time;
        item.count = mssg_itr->closed ? -1 : (item.count + 1);
        item.curatorsw = (fixp_t(sumcuratorsw_delta * curatorsw_factor)).data();
        item.rshares = rshares.data();
    });

    if (social_acc_param.account) {
        INLINE_ACTION_SENDER(golos::social, changereput)
            (social_acc_param.account, {social_acc_param.account, config::active_name},
            {voter, author, (rshares.data() >> 6)});
    }
}

uint16_t publication::notify_parent(bool increase, name parentacc, uint64_t parent_id) {
    tables::message_table message_table(_self, parentacc.value);
    auto mssg_itr = message_table.find(parent_id);
    eosio_assert(mssg_itr != message_table.end(), "Parent message doesn't exist");
    message_table.modify(mssg_itr, name(), [&]( auto &item ) {
        if (increase)
            ++item.childcount;
        else
            --item.childcount;
    });
    return mssg_itr->level;
}

void publication::fill_depleted_pool(tables::reward_pools& pools, eosio::asset quantity, tables::reward_pools::const_iterator excluded) {
    using namespace tables;
    using namespace structures;
    eosio_assert(quantity.amount >= 0, "fill_depleted_pool: quantity.amount < 0");
    if(quantity.amount == 0)
        return;
    auto choice = pools.end();
    auto min_ratio = std::numeric_limits<poolstate::ratio_t>::max();
    for(auto pool = pools.begin(); pool != pools.end(); ++pool)
        if((pool->state.funds.symbol == quantity.symbol) && (pool != excluded)) {
            auto cur_ratio = pool->state.get_ratio();
            if(cur_ratio <= min_ratio) {
                min_ratio = cur_ratio;
                choice = pool;
            }
        }
    //sic. we don't need assert here
    if(choice != pools.end())
        pools.modify(*choice, _self, [&](auto &item){ item.state.funds += quantity; });
}

void publication::on_transfer(name from, name to, eosio::asset quantity, std::string memo) {
    (void)memo;
    require_auth(from);
    if(_self != to)
        return;

    tables::reward_pools pools(_self, _self.value);
    fill_depleted_pool(pools, quantity, pools.end());
}

void publication::set_limit(std::string act_str, symbol_code token_code, uint8_t charge_id, int64_t price, int64_t cutoff, int64_t vesting_price, int64_t min_vesting) {
    using namespace tables;
    using namespace structures;
    require_auth(_self);
    eosio_assert(price < 0 || golos::charge::exist(config::charge_name, token_code, charge_id), "publication::set_limit: charge doesn't exist");
    auto act = limitparams::act_from_str(act_str);
    eosio_assert(act != limitparams::VOTE || charge_id == 0, "publication::set_limit: charge_id for VOTE should be 0");
    //? should we require cutoff to be _100percent if act == VOTE (to synchronize with vesting)?
    eosio_assert(act != limitparams::POSTBW || min_vesting == 0, "publication::set_limit: min_vesting for POSTBW should be 0");    
    golos::upsert_tbl<limit_table>(_self, _self.value, _self, act, [&](bool exists) {
        return [&](limitparams& item) {
            item.act = act;
            item.charge_id = charge_id;
            item.price = price;
            item.cutoff = cutoff;
            item.vesting_price = vesting_price;
            item.min_vesting = min_vesting;
        };
    });
}

void publication::set_rules(const funcparams& mainfunc, const funcparams& curationfunc, const funcparams& timepenalty,
    int64_t curatorsprop, int64_t maxtokenprop, eosio::symbol tokensymbol) {
    //TODO: machine's constants
    using namespace tables;
    using namespace structures;
    require_auth(_self);
    reward_pools pools(_self, _self.value);
    uint64_t created = current_time();

    eosio::asset unclaimed_funds = eosio::token::get_balance(config::token_name, _self, tokensymbol.code());

    auto old_pool = pools.begin();
    while(old_pool != pools.end())
        if(!old_pool->state.msgs)
            old_pool = pools.erase(old_pool);
        else {
            if(old_pool->state.funds.symbol == tokensymbol)
                unclaimed_funds -= old_pool->state.funds;
            ++old_pool;
        }
    eosio_assert(pools.find(created) == pools.end(), "rules with this key already exist");

    rewardrules newrules;
    atmsp::parser<fixp_t> pa;
    atmsp::machine<fixp_t> machine;

    newrules.mainfunc     = load_func(mainfunc, "reward func", pa, machine, true);
    newrules.curationfunc = load_func(curationfunc, "curation func", pa, machine, true);
    newrules.timepenalty  = load_func(timepenalty, "time penalty func", pa, machine, true);

    newrules.curatorsprop = static_cast<base_t>(get_limit_prop(curatorsprop).data());
    newrules.maxtokenprop = static_cast<base_t>(get_limit_prop(maxtokenprop).data());

    pools.emplace(_self, [&](auto &item) {
        item.created = created;
        item.rules = newrules;
        item.state.msgs = 0;
        item.state.funds = unclaimed_funds;
        item.state.rshares = (wdfp_t(0)).data();
        item.state.rsharesfn = (wdfp_t(0)).data();
    });
}

structures::funcinfo publication::load_func(const funcparams& params, const std::string& name, const atmsp::parser<fixp_t>& pa, atmsp::machine<fixp_t>& machine, bool inc) {
    eosio_assert(params.maxarg > 0, "forum::load_func: params.maxarg <= 0");
    structures::funcinfo ret;
    ret.maxarg = fp_cast<fixp_t>(params.maxarg).data();
    pa(machine, params.str, "x");
    check_positive_monotonic(machine, FP(ret.maxarg), name, inc);
    ret.code.from_machine(machine);
    return ret;
}

fixp_t publication::get_delta(atmsp::machine<fixp_t>& machine, fixp_t old_val, fixp_t new_val, const structures::funcinfo& func) {
    func.code.to_machine(machine);
    elap_t old_fn = machine.run({old_val}, {{fixp_t(0), FP(func.maxarg)}});
    elap_t new_fn = machine.run({new_val}, {{fixp_t(0), FP(func.maxarg)}});
    return fp_cast<fixp_t>(new_fn - old_fn, false);
}

void publication::check_account(name user, eosio::symbol tokensymbol) {
    eosio_assert(golos::vesting::balance_exist(config::vesting_name, user, tokensymbol.code()),
        ("unregistered user: " + name{user}.to_string()).c_str());
}

void publication::set_params(std::vector<posting_params> params) {
    require_auth(_self);
    posting_params_singleton cfg(_self, _self.value);
    param_helper::check_params(params, cfg.exists());
    param_helper::set_parameters<posting_params_setter>(params, cfg, _self);
}

void publication::reblog(name rebloger, name author, std::string permlink) {
    tables::message_table message_table(_self, author.value);
    auto message_id = hash64(permlink);
    eosio_assert(message_table.find(message_id) != message_table.end(), 
            "You can't reblog, because this message doesn't exist.");
}

} // golos
