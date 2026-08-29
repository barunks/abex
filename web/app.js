"use strict";

const state = {
  orders: new Map(),
  positions: {},
  health: {},
  balances: {},
  instrumentRules: {},
  marketQuotes: new Map(),
  marketBest: {},
  marketRing: {},
  marketSources: {},
  marketMaximumAgeMs: 5000,
  marketServerOffsetMs: 0,
  system: {
    transport: {}, exchangeConnectivity: {}, recoveryPolicy: {},
    persistence: {}, stability: {}, events: [],
  },
  socket: null,
  reconnectAttempt: 0,
  amendOrder: null,
  pipelineOrderId: null,
  orderSubmitting: false,
  ticketBlockReason: "Loading route preflight",
};

const $ = (selector) => document.querySelector(selector);
const elements = {
  orderForm: $("#orderForm"),
  clientOrderId: $("#clientOrderId"),
  orderType: $("#orderType"),
  symbol: $("#symbol"),
  priceField: $("#priceField"),
  priceHint: $("#priceHint"),
  notionalPreview: $("#notionalPreview"),
  fundingPreview: $("#fundingPreview"),
  fundingLabel: $("#fundingLabel"),
  fundingAvailable: $("#fundingAvailable"),
  fundingSuggested: $("#fundingSuggested"),
  fundingStatus: $("#fundingStatus"),
  submitOrder: $("#submitOrder"),
  ordersBody: $("#ordersBody"),
  ordersEmpty: $("#ordersEmpty"),
  orderSearch: $("#orderSearch"),
  statusFilter: $("#statusFilter"),
  venueList: $("#venueList"),
  venueConnectivityState: $("#venueConnectivityState"),
  positionList: $("#positionList"),
  marketGrid: $("#marketGrid"),
  marketFeedStatus: $("#marketFeedStatus"),
  marketSourceStatus: $("#marketSourceStatus"),
  modeBadge: $("#modeBadge"),
  activeTransport: $("#activeTransport"),
  exchangeIsolationState: $("#exchangeIsolationState"),
  exchangeRouteGrid: $("#exchangeRouteGrid"),
  recoveryFlow: $("#recoveryFlow"),
  stabilityState: $("#stabilityState"),
  stabilityAlert: $("#stabilityAlert"),
  recoveredOrders: $("#recoveredOrders"),
  retryCount: $("#retryCount"),
  reconciliationCount: $("#reconciliationCount"),
  journalSequence: $("#journalSequence"),
  durabilityState: $("#durabilityState"),
  journalLocation: $("#journalLocation"),
  eventCount: $("#eventCount"),
  eventList: $("#eventList"),
  connectionState: $("#connectionState"),
  amendDialog: $("#amendDialog"),
  amendForm: $("#amendForm"),
  amendOrderId: $("#amendOrderId"),
  amendPrice: $("#amendPrice"),
  amendQuantity: $("#amendQuantity"),
  pipelineDialog: $("#pipelineDialog"),
  pipelineOrderId: $("#pipelineOrderId"),
  pipelineSummary: $("#pipelineSummary"),
  pipelineList: $("#pipelineList"),
  toastStack: $("#toastStack"),
};

function nextClientId() {
  const time = Date.now().toString(36);
  const random = [...crypto.getRandomValues(new Uint32Array(2))]
    .map((value) => value.toString(36)).join("");
  // OKX clOrdId accepts only alphanumeric values (maximum 32 characters).
  return `ui${time}${random}`;
}

function requestId(prefix) {
  return `${prefix}-${Date.now().toString(36)}-${crypto.getRandomValues(new Uint16Array(1))[0].toString(36)}`;
}

async function api(path, options = {}) {
  const response = await fetch(path, {
    ...options,
    headers: { "Content-Type": "application/json", ...(options.headers || {}) },
  });
  const body = await response.json().catch(() => ({ ok: false, message: `HTTP ${response.status}` }));
  if (!response.ok || body.ok === false) {
    const error = new Error(body.message || body.code || "Gateway request failed");
    error.code = body.code;
    error.body = body;
    throw error;
  }
  return body;
}

function toast(title, message = "", kind = "success") {
  const node = document.createElement("div");
  node.className = `toast ${kind}`;
  const dot = document.createElement("i");
  const copy = document.createElement("div");
  const heading = document.createElement("strong");
  const detail = document.createElement("p");
  heading.textContent = title;
  detail.textContent = message;
  copy.append(heading, detail);
  node.append(dot, copy);
  elements.toastStack.append(node);
  setTimeout(() => node.remove(), 4800);
}

function setConnection(mode, label) {
  elements.connectionState.className = `connection ${mode}`;
  elements.connectionState.querySelector("span:last-child").textContent = label;
  elements.activeTransport.textContent = mode === "online"
    ? "WS LIVE · REST COMMANDS"
    : mode === "offline" ? "REST FALLBACK" : "REST + WS";
}

function setRuntimeMode(mode) {
  const normalized = String(mode || "unknown").toLowerCase();
  elements.modeBadge.className = `mode-badge ${normalized}`;
  elements.modeBadge.textContent = normalized === "live"
    ? "LIVE MODE"
    : normalized === "simulation" ? "SIMULATION" : "MODE UNKNOWN";
}

function formatNumber(value, fallback = "—") {
  if (value === undefined || value === null || value === "") return fallback;
  const number = Number(value);
  if (!Number.isFinite(number)) return String(value);
  return new Intl.NumberFormat(undefined, { maximumFractionDigits: 8 }).format(number);
}

function textCell(text, className = "") {
  const cell = document.createElement("td");
  cell.textContent = text;
  if (className) cell.className = className;
  return cell;
}

function marketKey(quote) {
  return `${quote.venue}:${quote.symbol}`;
}

function marketNow() {
  return Date.now() + state.marketServerOffsetMs;
}

function quoteAgeMs(quote) {
  if (!quote?.publishedAt) return Infinity;
  return Math.max(0, marketNow() - Number(quote.publishedAt));
}

function quoteIsFresh(quote) {
  return quoteAgeMs(quote) <= state.marketMaximumAgeMs;
}

function formatAge(milliseconds) {
  if (!Number.isFinite(milliseconds)) return "never";
  if (milliseconds < 1000) return `${Math.round(milliseconds)} ms`;
  if (milliseconds < 60000) return `${(milliseconds / 1000).toFixed(1)} s`;
  return `${Math.floor(milliseconds / 60000)}m ${Math.floor((milliseconds % 60000) / 1000)}s`;
}

function recomputeMarketBest() {
  const best = {};
  for (const quote of state.marketQuotes.values()) {
    if (!quoteIsFresh(quote)) continue;
    const current = best[quote.symbol] || {};
    if (!current.buy || Number(quote.ask) < Number(current.buy.price)) {
      current.buy = { venue: quote.venue, price: quote.ask };
    }
    if (!current.sell || Number(quote.bid) > Number(current.sell.price)) {
      current.sell = { venue: quote.venue, price: quote.bid };
    }
    best[quote.symbol] = current;
  }
  state.marketBest = best;
}

function applyMarketSnapshot(payload) {
  state.marketQuotes = new Map((payload.quotes || []).map((quote) => [marketKey(quote), quote]));
  state.marketRing = payload.ring || {};
  state.marketSources = payload.sources || {};
  state.marketMaximumAgeMs = Number(payload.maximumAgeMs || 5000);
  state.marketServerOffsetMs = Number(payload.serverTime || Date.now()) - Date.now();
  state.marketBest = payload.best || {};
  renderMarket();
}

function applySystemSnapshot(payload) {
  state.system = {
    transport: payload.transport || {},
    exchangeConnectivity: payload.exchangeConnectivity || {},
    recoveryPolicy: payload.recoveryPolicy || {},
    persistence: payload.persistence || {},
    stability: payload.stability || {},
    events: payload.events || [],
  };
  renderSystem();
}

function appendSystemEvent(event) {
  const bySequence = new Map((state.system.events || []).map((item) => [item.sequence, item]));
  bySequence.set(event.sequence, event);
  state.system.events = [...bySequence.values()]
    .sort((left, right) => left.sequence - right.sequence)
    .slice(-200);
  if (event.instanceId === state.system.stability.instanceId) {
    if (event.code === "IDEMPOTENT_REPLAY") {
      state.system.stability.idempotentReplays = Number(state.system.stability.idempotentReplays || 0) + 1;
    }
    if (event.code === "RECONCILIATION_STARTED") {
      state.system.stability.reconciliations = Number(state.system.stability.reconciliations || 0) + 1;
    }
    if (event.severity !== "INFO") {
      state.system.stability.alerts = Number(state.system.stability.alerts || 0) + 1;
    }
    state.system.persistence.recordSequence = Math.max(
      Number(state.system.persistence.recordSequence || 0), Number(event.sequence || 0),
    );
  }
  renderSystem();
}

function renderSystem() {
  const stability = state.system.stability || {};
  const persistence = state.system.persistence || {};
  const venues = Object.values(state.health);
  const disconnected = venues.find((venue) => !venue.connected);
  const degraded = venues.find((venue) => venue.reconciliationRequired);
  const loggingFailed = Number(stability.loggingFailures || 0) > 0;
  const ringMapped = Boolean(state.marketRing.mapped ?? state.marketRing.connected);
  const publisherAge = state.marketRing.lastUpdate
    ? Math.max(0, marketNow() - Number(state.marketRing.lastUpdate)) : Infinity;
  const publisherOnline = ringMapped && publisherAge <= state.marketMaximumAgeMs;
  const marketSourceOffline = ["OKX", "BINANCE"].find((venue) =>
    [...state.marketQuotes.values()].filter((quote) => quote.venue === venue && quoteIsFresh(quote)).length < 2);
  const currentEvents = (state.system.events || [])
    .filter((event) => event.instanceId === stability.instanceId);
  const latestAlert = [...currentEvents].reverse()
    .find((event) => event.severity === "CRITICAL" || event.severity === "WARNING");

  let status = "HEALTHY";
  let statusClass = "";
  let alertTitle = "No active stability alert";
  let alertMessage = "Retries are idempotent and restart recovery is journal-backed.";
  if (loggingFailed || disconnected || !publisherOnline) {
    status = "CRITICAL";
    statusClass = "critical";
    alertTitle = loggingFailed ? "Operational logging failure"
      : disconnected ? `${disconnected.venue || "Venue"} disconnected` : "Market-data publisher stale";
    alertMessage = stability.lastLoggingError || disconnected?.lastError
      || `No fresh mmap tick; last update was ${formatAge(publisherAge)} ago.`;
  } else if (degraded || marketSourceOffline) {
    status = "RECONCILE";
    statusClass = "warning";
    alertTitle = degraded ? `${degraded.venue || "Venue"} reconciliation required` : `${marketSourceOffline} market data incomplete`;
    alertMessage = degraded?.lastError
      || "Both BTC-USDT and ETH-USDT require fresh public market quotes.";
  } else if (latestAlert) {
    alertTitle = `Last ${latestAlert.severity.toLowerCase()} · ${latestAlert.code}`;
    alertMessage = latestAlert.message;
  }

  elements.stabilityState.className = `stability-state ${statusClass}`;
  elements.stabilityState.textContent = status;
  elements.stabilityAlert.className = `stability-alert ${statusClass}`;
  elements.stabilityAlert.querySelector("strong").textContent = alertTitle;
  elements.stabilityAlert.querySelector("span").textContent = alertMessage;
  elements.recoveredOrders.textContent = formatNumber(stability.recoveredOrders, "0");
  elements.retryCount.textContent = formatNumber(stability.idempotentReplays, "0");
  elements.reconciliationCount.textContent = formatNumber(stability.reconciliations, "0");
  elements.journalSequence.textContent = formatNumber(persistence.recordSequence, "0");
  elements.durabilityState.textContent = persistence.durableWrites ? "FDATASYNC" : "NON-DURABLE";
  elements.durabilityState.style.color = persistence.durableWrites ? "var(--green)" : "var(--amber)";
  elements.journalLocation.textContent = persistence.location || "—";
  elements.journalLocation.title = persistence.location || "";
  renderExchangeConnectivity();

  const events = [...(state.system.events || [])].sort((left, right) => right.sequence - left.sequence);
  elements.eventCount.textContent = `${events.length} EVENT${events.length === 1 ? "" : "S"}`;
  elements.eventList.replaceChildren();
  if (!events.length) {
    const empty = document.createElement("p");
    empty.className = "empty-small";
    empty.textContent = "No operational events loaded";
    elements.eventList.append(empty);
    return;
  }
  for (const event of events.slice(0, 100)) {
    const row = document.createElement("div");
    row.className = `event-row ${event.severity.toLowerCase()}`;
    const time = document.createElement("time");
    time.dateTime = new Date(event.occurredAt).toISOString();
    time.textContent = new Date(event.occurredAt).toLocaleTimeString([], { hour12: false });
    const severity = document.createElement("span");
    severity.className = "event-severity";
    severity.textContent = event.severity;
    const copy = document.createElement("div");
    copy.className = "event-copy";
    const code = document.createElement("strong");
    code.textContent = event.code;
    const message = document.createElement("span");
    message.textContent = event.message;
    copy.append(code, message);
    const context = document.createElement("span");
    context.className = "event-context";
    context.textContent = [event.venue, event.clientOrderId, event.requestId].filter(Boolean).join(" · ") || event.category;
    row.append(time, severity, copy, context);
    elements.eventList.append(row);
  }
}

function renderExchangeConnectivity() {
  const connectivity = state.system.exchangeConnectivity || {};
  const venueRoutes = connectivity.venues || {};
  const isolated = connectivity.orderGatewayIsolation === true;
  elements.exchangeIsolationState.className = `isolation-state ${isolated ? "" : "warning"}`;
  elements.exchangeIsolationState.textContent = isolated
    ? "ORDER GATEWAYS ISOLATED"
    : "NOT ISOLATED · OMS RESTART DOMAIN";
  elements.exchangeIsolationState.title = connectivity.isolationDetail || "";

  elements.exchangeRouteGrid.replaceChildren();
  for (const venueName of ["OKX", "BINANCE"]) {
    const route = venueRoutes[venueName] || {};
    const health = state.health[venueName] || {};
    const stateClass = health.connected
      ? (health.reconciliationRequired ? "degraded" : "online") : "offline";
    const card = document.createElement("article");
    card.className = `exchange-route ${stateClass}`;
    const header = document.createElement("header");
    const name = document.createElement("strong");
    name.textContent = venueName;
    const status = document.createElement("span");
    status.textContent = health.connected
      ? (health.reconciliationRequired ? "CONNECTED · RECONCILE" : "CONNECTED") : "OFFLINE";
    header.append(name, status);
    const details = document.createElement("dl");
    const addDetail = (label, primary, secondary = "") => {
      const term = document.createElement("dt");
      term.textContent = label;
      const description = document.createElement("dd");
      const value = document.createElement("strong");
      value.textContent = primary || "—";
      description.append(value);
      if (secondary) description.append(document.createElement("br"), secondary);
      details.append(term, description);
    };
    addDetail("Order commands", route.commands, route.commandOperations);
    addDetail("Execution updates", route.updates, route.updateDetail);
    addDetail("Heartbeat", route.heartbeat);
    addDetail("Stream reconnect", route.reconnect);
    addDetail("Process / restart", connectivity.processModel,
              connectivity.restartDomain || connectivity.isolationDetail);
    card.append(header, details);
    elements.exchangeRouteGrid.append(card);
  }

  const recovery = state.system.recoveryPolicy || {};
  const steps = [
    ["Persist", recovery.intentPersistence],
    ["Send", "Route once using the venue transport shown above"],
    ["Replay safely", recovery.requestReplay],
    ["Resolve unknown", recovery.transportFailure],
    ["Recover", recovery.reconnectRecovery || recovery.startupRecovery],
  ];
  elements.recoveryFlow.replaceChildren();
  steps.forEach(([label, detail], index) => {
    const step = document.createElement("div");
    step.className = "recovery-step";
    const number = document.createElement("b");
    number.textContent = String(index + 1).padStart(2, "0");
    const title = document.createElement("strong");
    title.textContent = label;
    const copy = document.createElement("small");
    copy.textContent = detail || "Waiting for OMS metadata";
    step.append(number, title, copy);
    elements.recoveryFlow.append(step);
  });
}

function marketValue(label, value, venue, className) {
  const node = document.createElement("div");
  node.className = `market-value ${className}`;
  const caption = document.createElement("small");
  caption.textContent = label;
  const price = document.createElement("strong");
  price.textContent = value ? formatNumber(value) : "—";
  const source = document.createElement("span");
  source.textContent = venue || "NO FRESH QUOTE";
  node.append(caption, price, source);
  return node;
}

function renderMarket() {
  recomputeMarketBest();
  const ringMapped = Boolean(state.marketRing.mapped ?? state.marketRing.connected);
  const publisherAge = state.marketRing.lastUpdate
    ? Math.max(0, marketNow() - Number(state.marketRing.lastUpdate)) : Infinity;
  const publisherOnline = ringMapped && publisherAge <= state.marketMaximumAgeMs;
  elements.marketFeedStatus.className = `market-feed-status ${publisherOnline ? "online" : "offline"}`;
  elements.marketFeedStatus.querySelector("span").textContent = publisherOnline
    ? `Publisher live · seq ${state.marketRing.lastSequence || 0} · ${formatAge(publisherAge)}`
    : ringMapped
      ? `Publisher stale · last tick ${formatAge(publisherAge)} ago`
      : state.marketRing.lastError || "Ring file unavailable";

  elements.marketSourceStatus.replaceChildren();
  const addSource = (label, detail, status) => {
    const node = document.createElement("div");
    node.className = `market-source ${status}`;
    const dot = document.createElement("i");
    const copy = document.createElement("span");
    const name = document.createElement("strong");
    const description = document.createElement("small");
    name.textContent = label;
    description.textContent = detail;
    copy.append(name, description);
    node.append(dot, copy);
    elements.marketSourceStatus.append(node);
  };
  addSource("MMAP RING", ringMapped ? "Mapped by OMS" : "Unavailable", ringMapped ? "online" : "offline");
  addSource("PUBLISHER", publisherOnline ? `Tick ${formatAge(publisherAge)} ago` : "No fresh tick",
            publisherOnline ? "online" : "offline");
  for (const venue of ["OKX", "BINANCE"]) {
    const venueQuotes = [...state.marketQuotes.values()].filter((quote) => quote.venue === venue);
    const freshCount = venueQuotes.filter(quoteIsFresh).length;
    const newestAge = venueQuotes.length ? Math.min(...venueQuotes.map(quoteAgeMs)) : Infinity;
    addSource(`${venue} MARKET`, `Public REST · ${freshCount}/2 · ${formatAge(newestAge)}`,
              freshCount === 2 ? "online" : freshCount ? "degraded" : "offline");
  }

  elements.marketGrid.replaceChildren();
  for (const symbol of ["BTC-USDT", "ETH-USDT"]) {
    const card = document.createElement("article");
    card.className = "market-card";
    const heading = document.createElement("header");
    const name = document.createElement("strong");
    name.textContent = symbol;
    const quoteCount = document.createElement("span");
    const venueQuotes = [...state.marketQuotes.values()].filter((quote) => quote.symbol === symbol);
    quoteCount.textContent = `${venueQuotes.filter(quoteIsFresh).length}/2 venues`;
    heading.append(name, quoteCount);

    const best = document.createElement("div");
    best.className = "market-best";
    const prices = state.marketBest[symbol] || {};
    best.append(
      marketValue("BEST BUY / ASK", prices.buy?.price, prices.buy?.venue, "buy"),
      marketValue("BEST SELL / BID", prices.sell?.price, prices.sell?.venue, "sell"),
    );

    const venueRows = document.createElement("div");
    venueRows.className = "market-venues";
    for (const venue of ["OKX", "BINANCE"]) {
      const quote = state.marketQuotes.get(`${venue}:${symbol}`);
      const row = document.createElement("div");
      const venueName = document.createElement("span");
      venueName.textContent = venue;
      const bid = document.createElement("span");
      bid.textContent = `BID ${quote ? formatNumber(quote.bid) : "—"}`;
      const ask = document.createElement("span");
      ask.textContent = `ASK ${quote ? formatNumber(quote.ask) : "—"}`;
      const freshness = document.createElement("span");
      const fresh = quoteIsFresh(quote);
      freshness.className = "quote-freshness";
      freshness.textContent = quote ? `${fresh ? "LIVE" : "STALE"} ${formatAge(quoteAgeMs(quote))}` : "NO DATA";
      if (!fresh) row.className = "stale";
      row.append(venueName, bid, ask, freshness);
      venueRows.append(row);
    }
    card.append(heading, best, venueRows);
    elements.marketGrid.append(card);
  }
  updateTicketQuote(false);
}

function currentTicketQuote() {
  const venue = elements.orderForm.querySelector('input[name="venue"]:checked')?.value;
  return state.marketQuotes.get(`${venue}:${elements.symbol.value}`);
}

function updateTicketQuote(forcePrice) {
  const quote = currentTicketQuote();
  const side = elements.orderForm.querySelector('input[name="side"]:checked')?.value;
  const executable = quoteIsFresh(quote) ? (side === "BUY" ? quote.ask : quote.bid) : null;
  elements.priceHint.textContent = executable
    ? `USDT · ${side === "BUY" ? "ask" : "bid"} ${formatNumber(executable)} on ${quote.venue}`
    : "USDT · no fresh quote";
  const price = elements.priceField.querySelector("input");
  if (elements.orderType.value === "LIMIT" && executable && (forcePrice || !price.value.trim())) {
    price.value = executable;
  }
  updateNotional();
}

function renderOrders() {
  const query = elements.orderSearch.value.trim().toLowerCase();
  const status = elements.statusFilter.value;
  const orders = [...state.orders.values()]
    .filter((order) => !status || order.status === status)
    .filter((order) => !query || `${order.clientOrderId} ${order.symbol} ${order.exchangeOrderId}`.toLowerCase().includes(query))
    .sort((a, b) => b.updatedAt - a.updatedAt);

  elements.ordersBody.replaceChildren();
  elements.ordersEmpty.hidden = orders.length > 0;
  for (const order of orders) {
    const row = document.createElement("tr");
    const identity = document.createElement("td");
    identity.className = "order-cell";
    const id = document.createElement("strong");
    id.textContent = order.clientOrderId;
    const symbol = document.createElement("small");
    symbol.textContent = order.symbol;
    identity.append(id, symbol);
    row.append(identity);
    row.append(textCell(order.venue));
    row.append(textCell(order.side, order.side === "BUY" ? "side-buy" : "side-sell"));
    row.append(textCell(`${order.type} · ${order.timeInForce}`));
    row.append(textCell(formatNumber(Number(order.price) || Number(order.averageFillPrice) || undefined, "MARKET")));
    row.append(textCell(formatNumber(order.quantity)));
    row.append(textCell(formatNumber(order.filledQuantity)));

    const statusCell = document.createElement("td");
    const badge = document.createElement("span");
    badge.className = `status-badge status-${order.status}`;
    badge.textContent = order.status.replace("_", " ");
    statusCell.append(badge);
    if (order.pendingAction && order.pendingAction !== "NONE") {
      const pending = document.createElement("small");
      pending.className = "pending";
      pending.textContent = `${order.pendingAction} PENDING`;
      statusCell.append(pending);
    }
    if (order.rejectionReason) {
      const rejection = document.createElement("small");
      rejection.className = "order-rejection";
      rejection.textContent = order.rejectionReason;
      rejection.title = order.rejectionReason;
      statusCell.append(rejection);
    }
    row.append(statusCell);

    const actionCell = document.createElement("td");
    const actions = document.createElement("div");
    actions.className = "row-actions";
    const pipeline = document.createElement("button");
    pipeline.type = "button";
    pipeline.className = "row-action";
    pipeline.textContent = "Pipeline";
    pipeline.addEventListener("click", () => openOrderPipeline(order));
    actions.append(pipeline);
    if (["LIVE", "PARTIALLY_FILLED"].includes(order.status) && order.pendingAction === "NONE") {
      if (order.type === "LIMIT") {
        const amend = document.createElement("button");
        amend.type = "button";
        amend.className = "row-action";
        amend.textContent = "Amend";
        amend.addEventListener("click", () => openAmend(order));
        actions.append(amend);
      }
      const cancel = document.createElement("button");
      cancel.type = "button";
      cancel.className = "row-action danger";
      cancel.textContent = "Cancel";
      cancel.addEventListener("click", () => cancelOrder(order));
      actions.append(cancel);
    }
    actionCell.append(actions);
    row.append(actionCell);
    elements.ordersBody.append(row);
  }
  renderMetrics();
}

function renderMetrics() {
  const orders = [...state.orders.values()];
  const open = orders.filter((order) => ["LIVE", "PARTIALLY_FILLED", "UNKNOWN"].includes(order.status));
  const filled = orders.filter((order) => order.status === "FILLED");
  const rejected = orders.filter((order) => order.status === "REJECTED");
  $("#openOrdersMetric").textContent = open.length;
  $("#filledMetric").textContent = filled.length;
  $("#rejectedMetric").textContent = rejected.length;
  const venueNames = ["OKX", "BINANCE"];
  const connectedNames = venueNames.filter((name) => state.health[name]?.connected);
  const healthyNames = connectedNames.filter((name) => !state.health[name]?.reconciliationRequired);
  const failedNames = venueNames.filter((name) => !state.health[name]?.connected);
  $("#healthMetric").textContent = healthyNames.length === venueNames.length
    ? "ONLINE" : `${connectedNames.length}/${venueNames.length}`;
  $("#healthTrend").textContent = healthyNames.length === venueNames.length
    ? "2/2 private sessions authenticated"
    : failedNames.length ? `${failedNames.join(" + ")} disconnected` : "Connected · reconciliation required";
}

function renderHealth() {
  const venueNames = ["OKX", "BINANCE"];
  const connected = venueNames.filter((name) => state.health[name]?.connected).length;
  const operational = venueNames.filter((name) =>
    state.health[name]?.connected && !state.health[name]?.reconciliationRequired).length;
  elements.venueConnectivityState.className = `venue-connectivity-state ${
    operational === venueNames.length ? "online" : connected ? "degraded" : "offline"}`;
  elements.venueConnectivityState.textContent = operational === venueNames.length
    ? `${operational}/${venueNames.length} CONNECTED`
    : connected ? `${connected}/${venueNames.length} · RECONCILE` : `0/${venueNames.length} OFFLINE`;
  elements.venueList.replaceChildren();
  for (const venueName of venueNames) {
    const health = state.health[venueName] || {};
    const card = document.createElement("div");
    const stateClass = health.connected ? (health.reconciliationRequired ? "degraded" : "online") : "offline";
    card.className = `venue-card ${stateClass}`;
    const identity = document.createElement("div");
    identity.className = "venue-identity";
    const logo = document.createElement("span");
    logo.className = "venue-logo";
    logo.textContent = venueName === "BINANCE" ? "BN" : "OK";
    const copy = document.createElement("div");
    const name = document.createElement("strong");
    name.textContent = venueName;
    const detail = document.createElement("small");
    detail.textContent = health.connected
      ? `Authenticated execution stream · ${health.sequenceGaps || 0} sequence gaps`
      : health.lastError
        ? `${venueName}: ${health.lastError}`
        : `${venueName}: disconnected`;
    copy.append(name, detail);
    identity.append(logo, copy);

    const status = document.createElement("div");
    status.className = "venue-status";
    const dot = document.createElement("i");
    const label = document.createElement("span");
    label.textContent = health.connected ? (health.reconciliationRequired ? "DEGRADED" : "CONNECTED") : "OFFLINE";
    const reconcile = document.createElement("button");
    reconcile.type = "button";
    reconcile.className = "reconcile-button";
    reconcile.textContent = "Reconcile";
    reconcile.addEventListener("click", () => reconcileVenue(venueName));
    status.append(dot, label, reconcile);
    card.append(identity, status);
    elements.venueList.append(card);
  }
  renderMetrics();
  renderSystem();
}

function renderPositions() {
  elements.positionList.replaceChildren();
  const positions = Object.entries(state.positions);
  if (!positions.length) {
    const empty = document.createElement("p");
    empty.className = "empty-small";
    empty.textContent = "No exposure";
    elements.positionList.append(empty);
    return;
  }
  const largest = Math.max(...positions.map(([, value]) => Math.abs(Number(value))), 1);
  for (const [symbol, value] of positions) {
    const negative = Number(value) < 0;
    const row = document.createElement("div");
    row.className = "position-row";
    const name = document.createElement("span");
    name.textContent = symbol;
    const amount = document.createElement("strong");
    amount.className = negative ? "negative" : "";
    amount.textContent = `${negative ? "" : "+"}${formatNumber(value)}`;
    const track = document.createElement("div");
    track.className = "position-track";
    const bar = document.createElement("i");
    bar.className = negative ? "negative" : "";
    bar.style.setProperty("--position-width", `${Math.max(3, Math.abs(Number(value)) / largest * 100)}%`);
    track.append(bar);
    row.append(name, amount, track);
    elements.positionList.append(row);
  }
}

function selectedFundingContext() {
  const form = new FormData(elements.orderForm);
  const venue = String(form.get("venue") || "");
  const symbol = String(form.get("symbol") || "");
  const side = String(form.get("side") || "");
  const [base, quote] = symbol.split("-");
  return { venue, symbol, side, base, quote, currency: side === "SELL" ? base : quote };
}

function fundingBalanceKey({ venue, currency }) {
  return `${venue}:${currency}`;
}

function instrumentRuleKey({ venue, symbol }) {
  return `${venue}:${symbol}`;
}

async function refreshSelectedBalance() {
  const context = selectedFundingContext();
  if (!context.venue || !context.currency) return;
  const key = fundingBalanceKey(context);
  if (!state.balances[key]) {
    state.balances[key] = {
      ok: false, venue: context.venue,
      message: `Loading ${context.currency} balance`, balances: [],
    };
    updateFundingPreview();
  }
  try {
    state.balances[key] = await api(
      `/api/v1/balances?venue=${encodeURIComponent(context.venue)}&currency=${encodeURIComponent(context.currency)}`,
    );
  } catch (error) {
    state.balances[key] = {
      ...(error.body || {}), ok: false, venue: context.venue,
      message: error.message || `${context.currency} balance unavailable`, balances: [],
    };
  }
  updateFundingPreview();
}

async function refreshSelectedRules() {
  const context = selectedFundingContext();
  if (!context.venue || !context.symbol) return;
  const key = instrumentRuleKey(context);
  if (state.instrumentRules[key]?.ok &&
      Date.now() - Number(state.instrumentRules[key].observedAt || 0) < 30000) {
    updateFundingPreview();
    return;
  }
  if (!state.instrumentRules[key]) {
    state.instrumentRules[key] = {
      ok: false, venue: context.venue, symbol: context.symbol,
      message: `Loading ${context.symbol} trading rules`,
    };
    updateFundingPreview();
  }
  try {
    state.instrumentRules[key] = await api(
      `/api/v1/instruments?venue=${encodeURIComponent(context.venue)}&symbol=${encodeURIComponent(context.symbol)}`,
    );
  } catch (error) {
    state.instrumentRules[key] = {
      ...(error.body || {}), ok: false, venue: context.venue, symbol: context.symbol,
      message: error.message || `${context.symbol} trading rules unavailable`,
    };
  }
  updateFundingPreview();
}

function pipelineTimestamp(milliseconds) {
  if (!milliseconds) return "—";
  return new Date(milliseconds).toLocaleTimeString([], {
    hour12: false, hour: "2-digit", minute: "2-digit", second: "2-digit",
    fractionalSecondDigits: 3,
  });
}

function renderOrderPipeline(payload) {
  const order = payload.order || {};
  elements.pipelineSummary.replaceChildren();
  const addSummary = (label, value) => {
    const item = document.createElement("span");
    const name = document.createElement("strong");
    name.textContent = `${label} `;
    item.append(name, value || "—");
    elements.pipelineSummary.append(item);
  };
  addSummary("VENUE", order.venue);
  addSummary("STATE", order.status);
  addSummary("EXCHANGE ID", order.exchangeOrderId || "Pending assignment");
  addSummary("FILLED", `${order.filledQuantity || "0"}/${order.quantity || "—"}`);
  if (order.averageFillPrice) addSummary("AVG FILL PRICE", formatNumber(order.averageFillPrice));

  const events = [...(payload.events || [])].sort((left, right) => left.sequence - right.sequence);
  elements.pipelineList.replaceChildren();
  if (!events.length) {
    const empty = document.createElement("p");
    empty.className = "empty-small";
    empty.textContent = "No durable pipeline events found";
    elements.pipelineList.append(empty);
    return;
  }
  for (const event of events) {
    const context = event.order || {};
    const row = document.createElement("article");
    row.className = "pipeline-event";
    const sequence = document.createElement("span");
    sequence.className = "pipeline-sequence";
    sequence.textContent = `#${String(event.sequence).padStart(6, "0")}`;
    const time = document.createElement("time");
    time.className = "pipeline-time";
    time.dateTime = new Date(event.occurredAt).toISOString();
    time.append(pipelineTimestamp(event.occurredAt));
    if (context.exchangeTime) {
      const exchangeTime = document.createElement("small");
      exchangeTime.textContent = `EXCH ${pipelineTimestamp(context.exchangeTime)}`;
      time.append(exchangeTime);
    }
    const stage = document.createElement("div");
    stage.className = "pipeline-stage";
    const code = document.createElement("strong");
    code.textContent = event.code;
    const severity = document.createElement("span");
    severity.className = event.severity.toLowerCase();
    severity.textContent = event.severity;
    stage.append(code, severity);
    const copy = document.createElement("div");
    copy.className = "pipeline-copy";
    const message = document.createElement("strong");
    message.textContent = event.message;
    const ids = document.createElement("span");
    ids.textContent = `CLIENT ${event.clientOrderId || payload.clientOrderId} · EXCHANGE ${context.exchangeOrderId || "PENDING"}`;
    const details = document.createElement("small");
    const orderDetails = [
      context.symbol,
      context.side,
      context.type,
      context.quantity ? `QTY ${context.quantity}` : "",
      context.price ? `PRICE ${context.price}` : context.averageFillPrice ? `AVG FILL ${context.averageFillPrice}` : context.type === "MARKET" ? "MARKET" : "",
      context.filledQuantity ? `FILLED ${context.filledQuantity}` : "",
      context.status ? `STATE ${context.status}` : "",
      context.pendingAction && context.pendingAction !== "NONE"
        ? `PENDING ${context.pendingAction}` : "",
      context.venueSequence ? `VENUE SEQ ${context.venueSequence}` : "",
      event.requestId ? `REQUEST ${event.requestId}` : "",
    ].filter(Boolean);
    details.textContent = orderDetails.join(" · ") || event.venue || event.category;
    copy.append(message, ids, details);
    const rejectionReason = context.rejectionReason ||
      (["WARNING", "CRITICAL"].includes(event.severity) ? event.message : "");
    if (rejectionReason) {
      const rejection = document.createElement("small");
      rejection.className = "pipeline-rejection";
      rejection.textContent = `REASON ${rejectionReason}`;
      copy.append(rejection);
    }
    row.append(sequence, time, stage, copy);
    elements.pipelineList.append(row);
  }
  elements.pipelineList.scrollTop = elements.pipelineList.scrollHeight;
}

async function loadOrderPipeline(clientOrderId) {
  try {
    const payload = await api(`/api/v1/orders/${encodeURIComponent(clientOrderId)}/pipeline`);
    if (state.pipelineOrderId === clientOrderId) renderOrderPipeline(payload);
  } catch (error) {
    if (state.pipelineOrderId !== clientOrderId) return;
    elements.pipelineList.replaceChildren();
    const failure = document.createElement("p");
    failure.className = "empty-small";
    failure.textContent = error.message;
    elements.pipelineList.append(failure);
  }
}

function openOrderPipeline(order) {
  state.pipelineOrderId = order.clientOrderId;
  elements.pipelineOrderId.textContent = order.clientOrderId;
  elements.pipelineSummary.replaceChildren();
  elements.pipelineList.replaceChildren();
  const loading = document.createElement("p");
  loading.className = "empty-small";
  loading.textContent = "Loading durable journal events…";
  elements.pipelineList.append(loading);
  if (!elements.pipelineDialog.open) elements.pipelineDialog.showModal();
  loadOrderPipeline(order.clientOrderId);
}

let pipelineRefreshTimer;
function refreshPipelineSoon(clientOrderId) {
  if (!clientOrderId || state.pipelineOrderId !== clientOrderId || !elements.pipelineDialog.open) return;
  clearTimeout(pipelineRefreshTimer);
  pipelineRefreshTimer = setTimeout(() => loadOrderPipeline(clientOrderId), 120);
}

async function refreshAll({ quiet = false } = {}) {
  try {
    const routePreflightPromise = Promise.all([
      refreshSelectedBalance(), refreshSelectedRules(),
    ]);
    const [orders, health, positions, market, system] = await Promise.all([
      api("/api/v1/orders"),
      api("/api/v1/health"),
      api("/api/v1/positions"),
      api("/api/v1/market-data"),
      api("/api/v1/system"),
    ]);
    await routePreflightPromise;
    state.orders = new Map(orders.orders.map((order) => [order.clientOrderId, order]));
    state.health = health.venues;
    setRuntimeMode(health.mode);
    state.positions = positions.positions;
    applyMarketSnapshot(market);
    applySystemSnapshot(system);
    renderOrders();
    renderHealth();
    renderPositions();
    if (!quiet) toast("Dashboard refreshed", "State reloaded from the gateway.");
  } catch (error) {
    if (!quiet) toast("Refresh failed", error.message, "error");
  }
}

function connectWebSocket() {
  if (state.socket && state.socket.readyState < WebSocket.CLOSING) state.socket.close();
  setConnection("", "Connecting");
  const protocol = location.protocol === "https:" ? "wss:" : "ws:";
  const socket = new WebSocket(`${protocol}//${location.host}/ws/v1/orders`);
  state.socket = socket;

  socket.addEventListener("open", () => {
    state.reconnectAttempt = 0;
    setConnection("online", "Live stream");
    socket.send(JSON.stringify({ type: "ping" }));
  });
  socket.addEventListener("message", (event) => {
    try {
      const message = JSON.parse(event.data);
      if (message.type === "orders.snapshot") {
        state.orders = new Map(message.orders.map((order) => [order.clientOrderId, order]));
        renderOrders();
      } else if (message.type === "order.updated") {
        state.orders.set(message.order.clientOrderId, message.order);
        renderOrders();
        refreshSecondarySoon();
      } else if (message.type === "market.snapshot") {
        applyMarketSnapshot(message);
      } else if (message.type === "market.updated") {
        state.marketQuotes.set(marketKey(message.quote), message.quote);
        state.marketRing.mapped = true;
        state.marketRing.connected = true;
        state.marketRing.lastSequence = message.quote.sequence;
        state.marketRing.lastUpdate = message.quote.publishedAt;
        recomputeMarketBest();
        renderMarket();
      } else if (message.type === "system.snapshot") {
        applySystemSnapshot(message);
      } else if (message.type === "system.event") {
        appendSystemEvent(message.event);
        refreshPipelineSoon(message.event.clientOrderId);
        if (["CONNECTIVITY", "RECONCILIATION"].includes(message.event.category)) {
          refreshSecondarySoon();
        }
        if (message.event.severity !== "INFO") {
          const venue = message.event.venue ? ` · ${message.event.venue}` : "";
          const isRepetitive = [
            "RECONCILIATION_INCOMPLETE",
            "OPEN_ORDER_SNAPSHOT_UNAVAILABLE",
            "RECONCILIATION_STARTED",
          ].includes(message.event.code);
          if (!isRepetitive) {
            toast(
              `${message.event.code}${venue}`,
              message.event.message,
              message.event.severity === "CRITICAL" ? "error" : "warning",
            );
          }
        }
      } else if (message.type === "resync.required") {
        refreshAll({ quiet: true });
      }
    } catch (_) {
      setConnection("offline", "Invalid stream");
    }
  });
  socket.addEventListener("close", () => {
    setConnection("offline", "Reconnecting");
    const delay = Math.min(15000, 500 * 2 ** state.reconnectAttempt++);
    setTimeout(connectWebSocket, delay);
  });
  socket.addEventListener("error", () => socket.close());
}

let secondaryRefreshTimer;
function refreshSecondarySoon() {
  clearTimeout(secondaryRefreshTimer);
  secondaryRefreshTimer = setTimeout(async () => {
    try {
      const [health, positions] = await Promise.all([api("/api/v1/health"), api("/api/v1/positions")]);
      state.health = health.venues;
      setRuntimeMode(health.mode);
      state.positions = positions.positions;
      renderHealth();
      renderPositions();
    } catch (_) { /* WebSocket reconnect and polling will recover. */ }
  }, 180);
}

async function submitOrder(event) {
  event.preventDefault();
  updateFundingPreview();
  if (state.ticketBlockReason) {
    toast("Order blocked before routing", state.ticketBlockReason, "warning");
    return;
  }
  const data = new FormData(elements.orderForm);
  const type = data.get("type");
  const order = {
    clientOrderId: data.get("clientOrderId").trim(),
    venue: data.get("venue"),
    symbol: data.get("symbol"),
    side: data.get("side"),
    type,
    quantity: data.get("quantity").trim(),
    timeInForce: data.get("timeInForce"),
  };
  if (type === "LIMIT") order.price = data.get("price").trim();
  // Rotate before network I/O so a rejected or failed attempt cannot be accidentally
  // resubmitted later with a different payload under the same idempotency identity.
  elements.clientOrderId.value = nextClientId();
  state.orderSubmitting = true;
  setTicketAvailability();
  try {
    const result = await api("/api/v1/orders", { method: "POST", body: JSON.stringify(order) });
    state.orders.set(result.order.clientOrderId, result.order);
    renderOrders();
    refreshSecondarySoon();
    await refreshSelectedBalance();
    toast(result.idempotentReplay ? "Order already exists" : "Order routed", `${order.clientOrderId} → ${order.venue}`);
  } catch (error) {
    if (error.body?.order) {
      state.orders.set(error.body.order.clientOrderId, error.body.order);
      renderOrders();
    }
    await refreshSelectedBalance();
    toast(error.code || "Order rejected", error.message, "error");
  } finally {
    state.orderSubmitting = false;
    updateFundingPreview();
  }
}

async function cancelOrder(order) {
  if (!confirm(`Cancel ${order.clientOrderId}?`)) return;
  try {
    const result = await api(`/api/v1/orders/${encodeURIComponent(order.clientOrderId)}`, {
      method: "DELETE",
      body: JSON.stringify({ requestId: requestId("cancel") }),
    });
    state.orders.set(result.order.clientOrderId, result.order);
    renderOrders();
    await refreshSelectedBalance();
    toast("Cancel accepted", order.clientOrderId);
  } catch (error) {
    toast(error.code || "Cancel failed", error.message, "error");
  }
}

function openAmend(order) {
  state.amendOrder = order;
  elements.amendOrderId.textContent = order.clientOrderId;
  elements.amendPrice.value = order.price || "";
  elements.amendQuantity.value = order.quantity;
  elements.amendDialog.showModal();
}

async function submitAmend(event) {
  event.preventDefault();
  if (event.submitter?.value === "cancel") {
    elements.amendDialog.close();
    return;
  }
  const order = state.amendOrder;
  const payload = { requestId: requestId("amend") };
  if (elements.amendPrice.value.trim()) payload.newPrice = elements.amendPrice.value.trim();
  if (elements.amendQuantity.value.trim()) payload.newQuantity = elements.amendQuantity.value.trim();
  try {
    const result = await api(`/api/v1/orders/${encodeURIComponent(order.clientOrderId)}`, {
      method: "PATCH",
      body: JSON.stringify(payload),
    });
    state.orders.set(result.order.clientOrderId, result.order);
    renderOrders();
    await refreshSelectedBalance();
    elements.amendDialog.close();
    toast("Amendment accepted", order.clientOrderId);
  } catch (error) {
    toast(error.code || "Amendment failed", error.message, "error");
  }
}

async function reconcileVenue(venue) {
  try {
    const result = await api(`/api/v1/reconcile/${venue}`, { method: "POST", body: "{}" });
    await refreshAll({ quiet: true });
    toast("Reconciliation complete", result.message || venue);
  } catch (error) {
    await refreshAll({ quiet: true });
    toast(error.code || "Reconciliation incomplete", error.message, "error");
  }
}

function updateOrderType() {
  const market = elements.orderType.value === "MARKET";
  const price = elements.priceField.querySelector("input");
  elements.priceField.classList.toggle("is-disabled", market);
  price.disabled = market;
  price.required = !market;
  updateTicketQuote(false);
}

function updateNotional() {
  const form = new FormData(elements.orderForm);
  const quantity = Number(form.get("quantity"));
  const quote = currentTicketQuote();
  const side = form.get("side");
  const marketPrice = quoteIsFresh(quote) ? Number(side === "BUY" ? quote.ask : quote.bid) : NaN;
  const price = elements.orderType.value === "MARKET" ? marketPrice : Number(form.get("price"));
  elements.notionalPreview.textContent = Number.isFinite(quantity * price) && quantity > 0 && price > 0
    ? `${formatNumber(quantity * price)} USDT`
    : elements.orderType.value === "MARKET" ? "Waiting for a fresh mapped quote" : "—";
  updateFundingPreview();
}

function decimalRaw(value) {
  const match = String(value ?? "").trim().match(/^([+-]?)(\d+)(?:\.(\d*))?$/);
  if (!match || (match[3] || "").length > 8) return null;
  const fraction = (match[3] || "").padEnd(8, "0");
  const raw = BigInt(match[2]) * 100000000n + BigInt(fraction || "0");
  return match[1] === "-" ? -raw : raw;
}

function exactMultiple(value, step) {
  if (!step || Number(step) <= 0) return true;
  const rawValue = decimalRaw(value);
  const rawStep = decimalRaw(step);
  return rawValue !== null && rawStep !== null && rawStep > 0n && rawValue % rawStep === 0n;
}

function ticketRuleViolation(form, rules, price) {
  if (!rules?.ok) return rules?.message || "Authoritative venue trading rules are unavailable";
  if (!rules.trading) return `${rules.symbol} is not tradable (${rules.status || "unknown status"})`;
  const quantityText = String(form.get("quantity") || "").trim();
  const quantity = Number(quantityText);
  if (!quantityText || !Number.isFinite(quantity) || quantity <= 0 || decimalRaw(quantityText) === null) {
    return "Enter a positive quantity with no more than 8 decimal places";
  }
  const type = String(form.get("type"));
  const minimumQuantity = Math.max(
    Number(rules.minimumQuantity || 0),
    type === "MARKET" ? Number(rules.marketMinimumQuantity || 0) : 0,
  );
  const maximumCandidates = [rules.maximumQuantity];
  if (type === "MARKET") maximumCandidates.push(rules.marketMaximumQuantity);
  const maximumQuantity = Math.min(...maximumCandidates
    .filter((value) => Number(value) > 0).map(Number), Infinity);
  if (quantity < minimumQuantity) return `Minimum ${type} quantity is ${formatNumber(minimumQuantity)} ${selectedFundingContext().base}`;
  if (quantity > maximumQuantity) return `Maximum ${type} quantity is ${formatNumber(maximumQuantity)} ${selectedFundingContext().base}`;
  if (!exactMultiple(quantityText, rules.quantityStep)) {
    return `Quantity must be an exact multiple of ${rules.quantityStep} ${selectedFundingContext().base}`;
  }
  if (type === "MARKET" && !exactMultiple(quantityText, rules.marketQuantityStep)) {
    return `MARKET quantity must be an exact multiple of ${rules.marketQuantityStep} ${selectedFundingContext().base}`;
  }

  if (!Number.isFinite(price) || price <= 0) return "A fresh executable price is required";
  if (type === "LIMIT") {
    const priceText = String(form.get("price") || "").trim();
    if (rules.minimumPrice && price < Number(rules.minimumPrice)) {
      return `Minimum LIMIT price is ${rules.minimumPrice} USDT`;
    }
    if (rules.maximumPrice && price > Number(rules.maximumPrice)) {
      return `Maximum LIMIT price is ${rules.maximumPrice} USDT`;
    }
    if (!exactMultiple(priceText, rules.priceTick)) {
      return `Price must be an exact multiple of ${rules.priceTick} USDT`;
    }
  }

  const notional = quantity * price;
  const minimumNotional = Number(type === "MARKET"
    ? rules.marketMinimumNotional || 0 : rules.minimumNotional || 0);
  const maximumNotional = Number(type === "MARKET"
    ? rules.marketMaximumNotional || Infinity : rules.maximumNotional || Infinity);
  if (notional < minimumNotional) return `Minimum ${type} amount is ${formatNumber(minimumNotional)} USDT`;
  if (notional > maximumNotional) return `Maximum ${type} amount is ${formatNumber(maximumNotional)} USDT`;
  return null;
}

function setTicketAvailability(blockReason = null) {
  state.ticketBlockReason = blockReason;
  elements.submitOrder.disabled = state.orderSubmitting || Boolean(blockReason);
  elements.submitOrder.title = blockReason || "Route validated order";
}

function updateFundingPreview() {
  const form = new FormData(elements.orderForm);
  const context = selectedFundingContext();
  const { venue, symbol, side, base, currency } = context;
  const snapshot = state.balances[fundingBalanceKey(context)];
  const rules = state.instrumentRules[instrumentRuleKey(context)];
  const reportedBalance = snapshot?.balances?.find((item) => item.currency === currency);
  const balance = reportedBalance || { available: "0", frozen: "0" };
  elements.fundingPreview.className = "funding-preview";
  elements.fundingLabel.textContent = `${venue || "VENUE"} ${symbol || "INSTRUMENT"} ${side || ""} · AVAILABLE ${currency || "BALANCE"}`;
  if (!snapshot?.ok) {
    elements.fundingAvailable.textContent = "—";
    elements.fundingSuggested.textContent = "—";
    elements.fundingStatus.textContent = snapshot?.message || `No ${currency || "currency"} balance reported`;
    setTicketAvailability(elements.fundingStatus.textContent);
    return;
  }
  if (!rules?.ok) {
    elements.fundingAvailable.textContent = `${formatNumber(balance.available)} ${currency}`;
    elements.fundingSuggested.textContent = "—";
    elements.fundingStatus.textContent = rules?.message || `Waiting for ${symbol} venue rules`;
    setTicketAvailability(elements.fundingStatus.textContent);
    return;
  }

  const available = Number(balance.available);
  const quantity = Number(form.get("quantity"));
  const ticketQuote = currentTicketQuote();
  const marketPrice = quoteIsFresh(ticketQuote)
    ? Number(side === "BUY" ? ticketQuote.ask : ticketQuote.bid) : NaN;
  const price = elements.orderType.value === "MARKET"
    ? marketPrice : Number(form.get("price"));
  const required = side === "SELL" ? quantity : quantity * price;
  const reserveFactor = 0.995;
  const unroundedMaximum = side === "SELL"
    ? available * reserveFactor
    : available * reserveFactor / price;
  const type = String(form.get("type"));
  const quantityStep = Math.max(
    Number(rules.quantityStep || 0),
    type === "MARKET" ? Number(rules.marketQuantityStep || 0) : 0,
    1e-8,
  );
  const minimumNotional = Number(type === "MARKET"
    ? rules.marketMinimumNotional || 0 : rules.minimumNotional || 0);
  const ruleMinimum = Math.max(
    Number(rules.minimumQuantity || 0),
    type === "MARKET" ? Number(rules.marketMinimumQuantity || 0) : 0,
    Number.isFinite(price) && price > 0 ? minimumNotional / price : 0,
  );
  const suggestedMinimum = Math.ceil((ruleMinimum - 1e-14) / quantityStep) * quantityStep;
  const maximumCandidates = [unroundedMaximum, Number(rules.maximumQuantity || Infinity)];
  if (type === "MARKET") maximumCandidates.push(Number(rules.marketMaximumQuantity || Infinity));
  const maximumNotional = Number(type === "MARKET"
    ? rules.marketMaximumNotional || Infinity : rules.maximumNotional || Infinity);
  if (Number.isFinite(price) && price > 0 && Number.isFinite(maximumNotional)) {
    maximumCandidates.push(maximumNotional / price);
  }
  const rawMaximum = Math.min(...maximumCandidates);
  const suggestedMaximum = Number.isFinite(rawMaximum) && rawMaximum > 0
    ? Math.floor((rawMaximum + 1e-14) / quantityStep) * quantityStep : 0;
  elements.fundingAvailable.textContent = `${formatNumber(balance.available)} ${currency}`;
  elements.fundingSuggested.textContent = Number.isFinite(rawMaximum)
    ? `${formatNumber(suggestedMinimum)}–${formatNumber(suggestedMaximum)} ${base}`
    : "PRICE REQUIRED";

  if (!Number.isFinite(rawMaximum)) {
    elements.fundingStatus.textContent = "A valid/fresh price is required to calculate a routeable quantity";
    setTicketAvailability(elements.fundingStatus.textContent);
  } else if (available <= 0 || suggestedMaximum < suggestedMinimum) {
    elements.fundingPreview.classList.add("insufficient");
    const minimumFunding = side === "SELL" ? suggestedMinimum : suggestedMinimum * price;
    elements.fundingStatus.textContent = `BLOCKED · available balance cannot meet venue minimum; needs about ${formatNumber(minimumFunding)} ${currency}`;
    setTicketAvailability(elements.fundingStatus.textContent);
  } else {
    const ruleViolation = ticketRuleViolation(form, rules, price);
    if (ruleViolation) {
      elements.fundingPreview.classList.add("insufficient");
      elements.fundingStatus.textContent = `BLOCKED · ${ruleViolation}`;
      setTicketAvailability(ruleViolation);
    } else if (available < required) {
      elements.fundingPreview.classList.add("insufficient");
      elements.fundingStatus.textContent = `BLOCKED · requires ${formatNumber(required)} ${currency}, available ${formatNumber(available)} ${currency}`;
      setTicketAvailability(elements.fundingStatus.textContent);
    } else {
      elements.fundingPreview.classList.add("sufficient");
      elements.fundingStatus.textContent = `READY · requires ${formatNumber(required)} ${currency} · venue minimum/step/tick checks passed`;
      setTicketAvailability();
    }
  }
}

function updateClock() {
  const now = new Date();
  $("#clock").textContent = now.toLocaleTimeString([], { hour12: false });
  $("#clockZone").textContent = Intl.DateTimeFormat().resolvedOptions().timeZone;
}

elements.orderForm.addEventListener("submit", submitOrder);
elements.orderForm.addEventListener("input", updateNotional);
elements.orderType.addEventListener("change", updateOrderType);
elements.symbol.addEventListener("change", () => {
  updateTicketQuote(true);
  refreshSelectedBalance();
  refreshSelectedRules();
});
for (const input of elements.orderForm.querySelectorAll('input[name="venue"], input[name="side"]')) {
  input.addEventListener("change", () => {
    updateTicketQuote(true);
    refreshSelectedBalance();
    refreshSelectedRules();
  });
}
elements.orderSearch.addEventListener("input", renderOrders);
elements.statusFilter.addEventListener("change", renderOrders);
elements.amendForm.addEventListener("submit", submitAmend);
elements.pipelineDialog.addEventListener("close", () => {
  state.pipelineOrderId = null;
  clearTimeout(pipelineRefreshTimer);
});
$("#refreshButton").addEventListener("click", () => refreshAll());

elements.clientOrderId.value = nextClientId();
updateOrderType();
updateClock();
setInterval(updateClock, 1000);
setInterval(renderMarket, 1000);
setInterval(() => refreshAll({ quiet: true }), 15000);
refreshAll({ quiet: true });
connectWebSocket();
