// Copyright [2020] <Copyright Kevin, kevin.lau.gd@gmail.com>

#include "trading_engine/trading_engine.h"

#include <spdlog/spdlog.h>

#include "core/contract_table.h"
#include "core/error_code.h"
#include "core/protocol.h"
#include "ipc/lockfree-queue/queue.h"
#include "ipc/redis_trader_cmd_helper.h"
#include "utils/misc.h"

namespace ft {

TradingEngine::TradingEngine() { risk_mgr_ = std::make_unique<RiskManager>(); }

TradingEngine::~TradingEngine() { close(); }

bool TradingEngine::login(const Config& config) {
  printf("***************TradingEngine****************\n");
  printf("* version: %lu\n", version());
  printf("* compiling time: %s %s\n", __TIME__, __DATE__);
  printf("********************************************\n");
  config.show();

  cmd_queue_key_ = config.key_of_cmd_queue;

  gateway_.reset(create_gateway(config.api));
  if (!gateway_) {
    spdlog::error("[TradingEngine::login] Failed. Unknown gateway");
    return false;
  }

  if (!gateway_->login(this, config)) {
    spdlog::error("[TradingEngine::login] Failed to login");
    return false;
  }
  spdlog::info("[TradingEngine::login] Success. Login as {}",
               config.investor_id);

  if (!gateway_->query_account()) {
    spdlog::error("[TradingEngine::login] Failed to query account");
    return false;
  }

  // query all positions
  portfolio_.set_account(account_.account_id);
  if (!gateway_->query_positions()) {
    spdlog::error("[TradingEngine::login] Failed to query positions");
    return false;
  }

  // query trades to update position
  if (!gateway_->query_trades()) {
    spdlog::error("[TradingEngine::login] Failed to query trades");
    return false;
  }

  // init risk manager
  if (!risk_mgr_->init(config, &account_, &portfolio_, &order_map_,
                       &md_snapshot_)) {
    spdlog::error("[TradingEngine::login] 风险管理对象初始化失败");
    return false;
  }

  // 启动个线程去定时查询资金账户信息
  if (config.api != "virtual") {
    std::thread([this]() {
      for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(15));
        gateway_->query_account();
      }
    }).detach();
  }

  spdlog::info("[TradingEngine::login] Init done");

  is_logon_ = true;
  return true;
}

void TradingEngine::process_cmd() {
  if (cmd_queue_key_ > 0)
    process_cmd_from_queue();
  else
    process_cmd_from_redis();
}

void TradingEngine::process_cmd_from_redis() {
  RedisTraderCmdPuller cmd_puller;
  cmd_puller.set_account(account_.account_id);
  spdlog::info("[TradingEngine::run] Start to recv cmd from topic: {}",
               cmd_puller.get_topic());

  for (;;) {
    auto reply = cmd_puller.pull();
    if (!reply) continue;

    auto cmd = reinterpret_cast<const TraderCommand*>(reply->element[2]->str);
    execute_cmd(*cmd);
  }
}

void TradingEngine::process_cmd_from_queue() {
  // 创建Queue的时候存在隐患，如果该key之前被其他应用创建且没有释放，
  // TradingEngine还是会依附在这个key的共享内存上，大概率导致内存访问越界。
  // 如果都是同版本的TradingEngine创建的则没有问题，可以重复使用。
  // 释放队列暂时需要手动释放，请使用ipcrm。
  // TODO(kevin): 在LFQueue_create和LFQueue_open的时候加上检验信息。 Done!

  // user_id用于验证是否是trading engine创建的queue
  uint32_t te_user_id = static_cast<uint32_t>(version());
  LFQueue* cmd_queue;
  if ((cmd_queue = LFQueue_open(cmd_queue_key_, te_user_id)) == nullptr) {
    int res = LFQueue_create(cmd_queue_key_, te_user_id, sizeof(TraderCommand),
                             4096 * 4, false);
    if (res != 0) {
      spdlog::info("TradingEngine::run_with_queue] Failed to create cmd queue");
      abort();
    }

    if ((cmd_queue = LFQueue_open(cmd_queue_key_, te_user_id)) == nullptr) {
      spdlog::info("TradingEngine::run_with_queue] Failed to open cmd queue");
      abort();
    }
  }

  LFQueue_reset(cmd_queue);
  spdlog::info("[TradingEngine::run] Start to recv cmd from queue: {:#x}",
               cmd_queue_key_);

  TraderCommand cmd{};
  int res;
  for (;;) {
    // 这里没有使用零拷贝的方式，性能影响甚微
    res = LFQueue_pop(cmd_queue, &cmd, nullptr, nullptr);
    if (res != 0) continue;

    execute_cmd(cmd);
  }
}

void TradingEngine::close() {
  if (gateway_) gateway_->logout();
}

void TradingEngine::execute_cmd(const TraderCommand& cmd) {
  if (cmd.magic != TRADER_CMD_MAGIC) {
    spdlog::error("[TradingEngine::run] Recv unknown cmd: error magic num");
    return;
  }

  switch (cmd.type) {
    case CMD_NEW_ORDER: {
      spdlog::debug("new order");
      send_order(cmd);
      break;
    }
    case CMD_CANCEL_ORDER: {
      spdlog::debug("cancel order");
      cancel_order(cmd.cancel_req.order_id);
      break;
    }
    case CMD_CANCEL_TICKER: {
      spdlog::debug("cancel all for ticker");
      cancel_for_ticker(cmd.cancel_ticker_req.ticker_index);
      break;
    }
    case CMD_CANCEL_ALL: {
      spdlog::debug("cancel all");
      cancel_all();
      break;
    }
    default: {
      spdlog::error("[StrategyEngine::run] Unknown cmd");
      break;
    }
  }
}

bool TradingEngine::send_order(const TraderCommand& cmd) {
  auto contract = ContractTable::get_by_index(cmd.order_req.ticker_index);
  if (!contract) {
    spdlog::error("[TradingEngine::send_order] Contract not found");
    return false;
  }

  Order order{};
  auto& req = order.req;
  req.engine_order_id = next_engine_order_id();
  req.contract = contract;
  req.direction = cmd.order_req.direction;
  req.offset = cmd.order_req.offset;
  req.volume = cmd.order_req.volume;
  req.type = cmd.order_req.type;
  req.price = cmd.order_req.price;
  req.flags = cmd.order_req.flags;
  order.user_order_id = cmd.order_req.user_order_id;
  order.status = OrderStatus::SUBMITTING;
  order.strategy_id = cmd.strategy_id;

  std::unique_lock<std::mutex> lock(mutex_);
  // 增加是否经过风控检查字段，在紧急情况下可以设置该字段绕过风控下单
  if (!cmd.order_req.without_check) {
    int error_code = risk_mgr_->check_order_req(&order);
    if (error_code != NO_ERROR) {
      spdlog::error("[TradingEngine::send_order] 风控未通过: {}",
                    error_code_str(error_code));
      risk_mgr_->on_order_rejected(&order, error_code);
      return false;
    }
  }

  if (!gateway_->send_order(req)) {
    spdlog::error(
        "[StrategyEngine::send_order] Failed to send_order. {}, {}{}, {}, "
        "Volume:{}, Price:{:.3f}",
        contract->ticker, direction_str(req.direction), offset_str(req.offset),
        ordertype_str(req.type), req.volume, req.price);

    risk_mgr_->on_order_rejected(&order, ERR_SEND_FAILED);
    return false;
  }

  order_map_.emplace((uint64_t)req.engine_order_id, order);
  risk_mgr_->on_order_sent(&order);

  spdlog::debug(
      "[StrategyEngine::send_order] Success. {}, {}{}, {}, EngineOrderID:{}, "
      "Volume:{}, Price: {:.3f}",
      contract->ticker, direction_str(req.direction), offset_str(req.offset),
      ordertype_str(req.type), req.engine_order_id, req.volume, req.price);
  return true;
}

void TradingEngine::cancel_order(uint64_t order_id) {
  gateway_->cancel_order(order_id);
}

void TradingEngine::cancel_for_ticker(uint32_t ticker_index) {
  std::unique_lock<std::mutex> lock(mutex_);
  for (const auto& [engine_order_id, order] : order_map_) {
    UNUSED(engine_order_id);
    if (ticker_index == order.req.contract->index)
      gateway_->cancel_order(order.order_id);
  }
}

void TradingEngine::cancel_all() {
  std::unique_lock<std::mutex> lock(mutex_);
  for (const auto& [engine_order_id, order] : order_map_) {
    UNUSED(engine_order_id);
    gateway_->cancel_order(order.order_id);
  }
}

void TradingEngine::on_query_contract(Contract* contract) {}

void TradingEngine::on_query_account(Account* account) {
  std::unique_lock<std::mutex> lock(mutex_);
  account_ = *account;
  lock.unlock();

  spdlog::info(
      "[TradingEngine::on_query_account] total_asset:{:.3f}, frozen:{:.3f}, "
      "margin:{:.3f}",
      account->total_asset, account->frozen, account->margin);
}

void TradingEngine::on_query_position(Position* position) {
  auto contract = ContractTable::get_by_index(position->ticker_index);
  assert(contract);

  auto& lp = position->long_pos;
  auto& sp = position->short_pos;
  spdlog::info(
      "[TradingEngine::on_query_position] {}, LongVol:{}, LongYdVol:{}, "
      "LongPrice:{:.2f}, LongFrozen:{}, LongPNL:{}, ShortVol:{}, "
      "ShortYdVol:{}, ShortPrice:{:.2f}, ShortFrozen:{}, ShortPNL:{}",
      contract->ticker, lp.holdings, lp.yd_holdings, lp.cost_price, lp.frozen,
      lp.float_pnl, sp.holdings, sp.yd_holdings, sp.cost_price, sp.frozen,
      sp.float_pnl);

  if (lp.holdings == 0 && lp.frozen == 0 && sp.holdings == 0 && sp.frozen == 0)
    return;

  portfolio_.set_position(*position);
}

void TradingEngine::on_tick(TickData* tick) {
  if (!is_logon_) return;

  auto contract = ContractTable::get_by_index(tick->ticker_index);
  assert(contract);
  md_pusher_.push(contract->ticker, *tick);

  md_snapshot_.update_snapshot(*tick);
  spdlog::trace("[TradingEngine::process_tick] {}  ask:{:.3f}  bid:{:.3f}",
                contract->ticker, tick->ask[0], tick->bid[0]);
}

void TradingEngine::on_query_trade(OrderTradedRsp* trade) {
  portfolio_.update_on_query_trade(trade->ticker_index, trade->direction,
                                   trade->offset, trade->volume);
}

/*
 * 订单被市场接受后通知策略
 * 告知策略order_id，策略可通过此order_id撤单
 */
void TradingEngine::on_order_accepted(OrderAcceptedRsp* rsp) {
  std::unique_lock<std::mutex> lock(mutex_);
  auto iter = order_map_.find(rsp->engine_order_id);
  if (iter == order_map_.end()) {
    spdlog::warn(
        "[TradingEngine::on_order_accepted] Order not found. OrderID: {}",
        rsp->engine_order_id);
    return;
  }

  auto& order = iter->second;
  if (order.accepted) return;

  order.order_id = rsp->order_id;
  order.accepted = true;
  risk_mgr_->on_order_accepted(&order);

  spdlog::info(
      "[TradingEngine::on_order_accepted] 报单委托成功. {}, {}{}, Volume:{}, "
      "Price:{:.2f}, OrderType:{}",
      order.req.contract->ticker, direction_str(order.req.direction),
      offset_str(order.req.offset), order.req.volume, order.req.price,
      ordertype_str(order.req.type));
}

void TradingEngine::on_order_rejected(OrderRejectedRsp* rsp) {
  std::unique_lock<std::mutex> lock(mutex_);
  auto iter = order_map_.find(rsp->engine_order_id);
  if (iter == order_map_.end()) {
    spdlog::warn(
        "[TradingEngine::on_order_rejected] Order not found. OrderID: {}",
        rsp->engine_order_id);
    return;
  }

  auto& order = iter->second;
  risk_mgr_->on_order_rejected(&order, ERR_REJECTED);

  spdlog::error(
      "[TradingEngine::on_order_rejected] 报单被拒：{}. {}, {}{}, Volume:{}, "
      "Price:{:.3f}",
      rsp->reason, order.req.contract->ticker,
      direction_str(order.req.direction), offset_str(order.req.offset),
      order.req.volume, order.req.price);

  order_map_.erase(iter);
}

void TradingEngine::on_order_traded(OrderTradedRsp* rsp) {
  if (rsp->trade_type == TradeType::SECONDARY_MARKET)
    on_secondary_market_traded(rsp);
  else
    on_primary_market_traded(rsp);
}

void TradingEngine::on_primary_market_traded(OrderTradedRsp* rsp) {
  std::unique_lock<std::mutex> lock(mutex_);
  auto iter = order_map_.find(rsp->engine_order_id);
  if (iter == order_map_.end()) {
    spdlog::warn(
        "[TradingEngine::on_primary_market_traded] Order not found. "
        "OrderID:{}, Traded:{}, Price:{:.3f}",
        rsp->order_id, rsp->volume, rsp->price);
    return;
  }

  auto& order = iter->second;
  if (!order.accepted) {
    order.accepted = true;
    risk_mgr_->on_order_accepted(&order);

    spdlog::info(
        "[TradingEngine::on_order_accepted] 报单委托成功. {}, {}, "
        "OrderID:{}, Volume:{}",
        order.req.contract->ticker, direction_str(order.req.direction),
        order.order_id, order.req.volume);
  }

  order.order_id = rsp->order_id;
  if (rsp->trade_type == TradeType::ACQUIRED_STOCK) {
    risk_mgr_->on_order_traded(&order, rsp);
  } else if (rsp->trade_type == TradeType::RELEASED_STOCK) {
    risk_mgr_->on_order_traded(&order, rsp);
  } else if (rsp->trade_type == TradeType::CASH_SUBSTITUTION) {
    risk_mgr_->on_order_traded(&order, rsp);
  } else if (rsp->trade_type == TradeType::PRIMARY_MARKET) {
    order.traded_volume = rsp->volume;
    risk_mgr_->on_order_traded(&order, rsp);
    // risk_mgr_->on_order_completed(&order);
    spdlog::info(
        "[TradingEngine::on_primary_market_traded] done. {}, {}, Volume:{}",
        order.req.contract->ticker, direction_str(order.req.direction),
        order.req.volume);
    order_map_.erase(iter);
  }
}

void TradingEngine::on_secondary_market_traded(OrderTradedRsp* rsp) {
  std::unique_lock<std::mutex> lock(mutex_);
  auto iter = order_map_.find(rsp->engine_order_id);
  if (iter == order_map_.end()) {
    spdlog::warn(
        "[TradingEngine::on_secondary_market_traded] Order not found. "
        "OrderID:{}, Traded:{}, Price:{:.3f}",
        rsp->order_id, rsp->volume, rsp->price);
    return;
  }

  auto& order = iter->second;
  if (!order.accepted) {
    order.accepted = true;
    risk_mgr_->on_order_accepted(&order);

    spdlog::info(
        "[TradingEngine::on_order_accepted] 报单委托成功. {}, {}{}, Volume:{}, "
        "Price:{:.2f}, OrderType:{}",
        order.req.contract->ticker, direction_str(order.req.direction),
        offset_str(order.req.offset), order.req.volume, order.req.price,
        ordertype_str(order.req.type));
  }

  order.order_id = rsp->order_id;
  order.traded_volume += rsp->volume;

  spdlog::info(
      "[TradingEngine::on_order_traded] 报单成交. {}, {}{}, Traded:{}, "
      "Price:{:.3f}, TotalTraded/Original:{}/{}",
      order.req.contract->ticker, direction_str(order.req.direction),
      offset_str(order.req.offset), rsp->volume, rsp->price,
      order.traded_volume, order.req.volume);

  risk_mgr_->on_order_traded(&order, rsp);

  if (order.traded_volume + order.canceled_volume == order.req.volume) {
    spdlog::info(
        "[TradingEngine::on_order_traded] 报单完成. {}, {}{}, OrderID:{}, "
        "Traded/Original: {}/{}",
        order.req.contract->ticker, direction_str(order.req.direction),
        offset_str(order.req.offset), order.order_id, order.traded_volume,
        order.req.volume);

    // 订单结束，通知风控模块
    risk_mgr_->on_order_completed(&order);
    order_map_.erase(iter);
  }
}

void TradingEngine::on_order_canceled(OrderCanceledRsp* rsp) {
  std::unique_lock<std::mutex> lock(mutex_);
  auto iter = order_map_.find(rsp->engine_order_id);
  if (iter == order_map_.end()) {
    spdlog::warn(
        "[TradingEngine::on_order_canceled] Order not found. EngineOrderID:{}",
        rsp->engine_order_id);
    return;
  }

  auto& order = iter->second;
  order.canceled_volume = rsp->canceled_volume;

  spdlog::info(
      "[TradingEngine::on_order_canceled] 报单已撤. {}, {}{}, OrderID:{}, "
      "Canceled:{}",
      order.req.contract->ticker, direction_str(order.req.direction),
      offset_str(order.req.offset), order.order_id, rsp->canceled_volume);

  risk_mgr_->on_order_canceled(&order, rsp->canceled_volume);

  if (order.traded_volume + order.canceled_volume == order.req.volume) {
    spdlog::info(
        "[TradingEngine::on_order_canceled] 报单完成. {}, {}{}, OrderID:{}, "
        "Traded/Original:{}/{}",
        order.req.contract->ticker, direction_str(order.req.direction),
        offset_str(order.req.offset), order.order_id, order.traded_volume,
        order.req.volume);

    risk_mgr_->on_order_completed(&order);
    order_map_.erase(iter);
  }
}

void TradingEngine::on_order_cancel_rejected(OrderCancelRejectedRsp* rsp) {
  spdlog::warn(
      "[TradingEngine::on_order_cancel_rejected] 订单不可撤：{}. "
      "EngineOrderID: {}",
      rsp->reason, rsp->engine_order_id);
}

}  // namespace ft
