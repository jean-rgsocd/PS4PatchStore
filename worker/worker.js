// PS4 Patch Store - servidor de login (Cloudflare Worker)
// Precisa de: KV namespace ligado como "DB"; secrets TOKEN_SECRET e ADMIN_KEY.
// R2: bucket ligado como "APP_BUCKET".
//
// App:
// POST /login    (user, pass, dev?)  -> "OK\ntoken\nexp\nplano|estado[\nLOCKED]"
// POST /register (user, pass, dev?)  -> "OK\ncadastrado\nestado\nvence(seg)"
// GET  /me       (Authorization: Bearer <token> ou ?t=<token>)
// GET  /plans    (publico: precos, prazos, Pix e WhatsApp dos planos)
// GET  /preview  (sem login, catalogo sem links de download)
// GET  /catalog  (Authorization: Bearer <token> ou ?t=<token>)
//
// Admin: GET /admin
//
// ACESSO (planos pagos):
// - Cadastro novo com "dev" (ID do console): conta ativa por 1 HORA (teste), uma vez por console.
//   Console que ja teve o teste: conta nasce vencida (so ve o catalogo).
// - Cadastro sem "dev" (app antigo): nasce pendente, ativa manual pelo painel.
// - Planos: mensal 30d, semestral 180d, anual 365d (painel: botoes "+ Mensal" etc.; somam ao prazo atual).
// - Vencido ou pendente: catalogo "so olhar" (sem links), igual ao visitante.
// - Conta antiga (sem "v2") continua sem vencimento ate a migracao ser aplicada no painel.
// - Conta presa ao console (dev): outro console e recusado ate o admin liberar.

const enc = new TextEncoder();

const TOKEN_DAYS = 30;

const MAX_FAILS = 5;
const LOCK_SECONDS = 900;
const LOG_TTL = 60 * 60 * 24 * 90;

// 1 cadastro por console (ID do PS4). Por endereco de internet (IP): ate REG_MAX_PER_IP contas novas a cada 24 h.
const REG_MAX_PER_IP = 3;
const REG_IP_TTL = 86400;
const REG_MAX_PENDING = 200;

const HOUR_MS = 3600 * 1000;
const DAY_MS = 24 * HOUR_MS;
const TRIAL_MS = HOUR_MS;

// ---------- PLANOS (precos e Pix: edite aqui e publique o worker) ----------
const WHATSAPP = "5511984970539";
const PLANOS = {
  mensal:    { nome: "Mensal",    dias: 30,  preco: "15.00",  pix: "00020126580014br.gov.bcb.pix0136c0c3ac52-ac89-4f67-998d-cfa0f1d25530520400005303986540515.005802BR5925JEAN CARLOS RIBEIRO DA SI6009SAO PAULO62070503***6304AE00" },
  semestral: { nome: "Semestral", dias: 180, preco: "80.00",  pix: "00020126580014br.gov.bcb.pix0136c0c3ac52-ac89-4f67-998d-cfa0f1d25530520400005303986540580.005802BR5925JEAN CARLOS RIBEIRO DA SI6009SAO PAULO62070503***63041C06" },
  anual:     { nome: "Anual",     dias: 365, preco: "120.00", pix: "00020126580014br.gov.bcb.pix0136c0c3ac52-ac89-4f67-998d-cfa0f1d255305204000053039865406120.005802BR5925JEAN CARLOS RIBEIRO DA SI6009SAO PAULO62070503***6304A1E9" }
};

// Estado da conta: pendente, ativo, vencido ou desativado.
// Conta sem "v2" e conta antiga: sem vencimento (o campo expires antigo e ignorado).
function accountState(rec, now) {
  if (!rec) return "";
  if (rec.pending) return "pendente";
  if (rec.disabled) return "desativado";
  if (rec.v2 && rec.expires) {
    const t = Date.parse(rec.expires);
    if (t && t <= (now || Date.now())) return "vencido";
  }
  return "ativo";
}

// Sem acesso aos downloads: pendente e vencido veem o catalogo "so olhar".
// Depois que o app novo (planos) estiver publicado, troque para true: app antigo passa a ver so o catalogo "so olhar"
// (com a linha APP, entao ele se atualiza sozinho). O app novo manda o cabecalho X-JK-App.
const EXIGIR_APP_NOVO = false;

const isLocked = (st) => st === "pendente" || st === "vencido";

// Grava o usuario e mantem os metadados do painel atualizados.
async function putUser(env, user, rec, last) {
  await env.DB.put("user:" + user, JSON.stringify(rec), {
    metadata: {
      disabled: !!rec.disabled,
      pending: !!rec.pending,
      created: rec.created || "",
      expires: rec.expires || "",
      v2: rec.v2 ? 1 : 0,
      plan: rec.plan || "",
      dev: rec.dev ? 1 : 0,
      last: last || "",
      sup: rec.support_until || "",
      nag: !!rec.nag,
      ips: (rec.ips || []).map((x) => [x.ip, x.t, x.c])
    }
  });
}

async function userMeta(env, user) {
  const l = await env.DB.list({ prefix: "user:" + user });
  const k = l.keys.find((x) => x.name === "user:" + user);
  return (k && k.metadata) || {};
}

const b64u = (buf) =>
  btoa(String.fromCharCode(...new Uint8Array(buf)))
    .replace(/\+/g, "-")
    .replace(/\//g, "_")
    .replace(/=+$/, "");

const fromB64u = (s) =>
  Uint8Array.from(
    atob(s.replace(/-/g, "+").replace(/_/g, "/")),
    (c) => c.charCodeAt(0)
  );

async function hmac(secret, data) {
  const key = await crypto.subtle.importKey(
    "raw",
    enc.encode(secret),
    { name: "HMAC", hash: "SHA-256" },
    false,
    ["sign"]
  );

  return new Uint8Array(
    await crypto.subtle.sign("HMAC", key, enc.encode(data))
  );
}

function safeEqual(a, b) {
  const x = typeof a === "string" ? enc.encode(a) : a;
  const y = typeof b === "string" ? enc.encode(b) : b;

  if (x.length !== y.length) return false;

  let d = 0;

  for (let i = 0; i < x.length; i++) {
    d |= x[i] ^ y[i];
  }

  return d === 0;
}

const text = (status, body) =>
  new Response(body, {
    status,
    headers: {
      "Content-Type": "text/plain; charset=utf-8",
      "Cache-Control": "no-store"
    }
  });

const json = (status, obj) =>
  new Response(JSON.stringify(obj), {
    status,
    headers: {
      "Content-Type": "application/json; charset=utf-8",
      "Cache-Control": "no-store"
    }
  });

async function hashPass(env, salt, pass) {
  return b64u(
    await hmac(env.TOKEN_SECRET, "pw:" + salt + ":" + pass)
  );
}

// ID do console (OpenPSID enviado pelo app): so o hash fica guardado.
function cleanDev(v) {
  const s = String(v || "").trim().toLowerCase();
  return /^[0-9a-f]{16,64}$/.test(s) && !/^(0+|f+)$/.test(s) ? s : "";
}

async function hashDev(env, dev) {
  return b64u(await hmac(env.TOKEN_SECRET, "dev:" + dev)).slice(0, 22);
}

async function makeToken(env, user) {
  const exp = Math.floor(Date.now() / 1000) + TOKEN_DAYS * 86400;

  const payload = b64u(
    enc.encode(JSON.stringify({ u: user, exp }))
  );

  const sig = b64u(
    await hmac(env.TOKEN_SECRET, "tk:" + payload)
  );

  return {
    token: payload + "." + sig,
    exp
  };
}

async function readToken(env, token) {
  try {
    const [payload, sig] = (token || "").split(".");

    if (!payload || !sig) return null;

    const good = b64u(
      await hmac(env.TOKEN_SECRET, "tk:" + payload)
    );

    if (!safeEqual(good, sig)) return null;

    const data = JSON.parse(
      new TextDecoder().decode(fromB64u(payload))
    );

    if (!data.u || data.exp < Math.floor(Date.now() / 1000)) {
      return null;
    }

    return data.u;
  } catch (e) {
    return null;
  }
}

async function readForm(request) {
  const ct = request.headers.get("Content-Type") || "";
  const raw = await request.text();

  if (ct.includes("json")) {
    try {
      return JSON.parse(raw);
    } catch (e) {
      return {};
    }
  }

  return Object.fromEntries(new URLSearchParams(raw));
}

// Guarda os ultimos IPs distintos de login.
const MAX_IPS = 5;

function trackIp(rec, request) {
  const ip = (
    request.headers.get("CF-Connecting-IP") || ""
  ).slice(0, 45);

  if (!ip) return;

  const c = String(
    (request.cf && request.cf.country) || ""
  ).slice(0, 2);

  const a = (rec.ips || []).filter((x) => x.ip !== ip);

  a.unshift({
    ip,
    t: new Date(Date.now()).toISOString(),
    c
  });

  rec.ips = a.slice(0, MAX_IPS);
}

async function logEvent(env, user, result, request) {
  const key =
    "log:" +
    String(9999999999999 - Date.now()).padStart(13, "0") +
    ":" +
    user;

  const meta = {
    user,
    result,
    t: new Date(Date.now()).toISOString(),
    ip: request.headers.get("CF-Connecting-IP") || "",
    country: (request.cf && request.cf.country) || ""
  };

  try {
    await env.DB.put(key, "1", {
      metadata: meta,
      expirationTtl: LOG_TTL
    });
  } catch (e) {
    // O log nunca pode impedir o login.
  }
}

// Evita gravacoes repetidas do mesmo IP em menos de 6 horas.
const RECENT_LOGIN_MS = 6 * 3600 * 1000;

function recentSameIp(rec, request) {
  const ip = (
    request.headers.get("CF-Connecting-IP") || ""
  ).slice(0, 45);

  const a = rec.ips && rec.ips[0];

  return !!(
    a &&
    ip &&
    a.ip === ip &&
    Date.now() - (Date.parse(a.t) || 0) < RECENT_LOGIN_MS
  );
}

// Linha do plano mostrada ao app (compatibilidade: app antigo so entende "pendente").
function planLine(rec, st) {
  if (st === "pendente" || st === "vencido") return "pendente";
  return rec.v2 && rec.expires ? rec.expires : "ilimitado";
}

// LOGIN
async function login(request, env) {
  const f = await readForm(request);

  const user = String(f.user || "")
    .trim()
    .toLowerCase()
    .slice(0, 32);

  const pass = String(f.pass || "").slice(0, 128);
  const dev = cleanDev(f.dev);

  if (!user || !pass) {
    return text(400, "ERRO\ninforme usuario e senha");
  }

  const rec = await env.DB.get("user:" + user, "json");

  if (!rec || (rec.disabled && !rec.pending)) {
    return text(401, "ERRO\nusuario ou senha invalidos");
  }

  const fails = parseInt(
    (await env.DB.get("fail:" + user)) || "0",
    10
  );

  if (fails >= MAX_FAILS) {
    return text(429, "ERRO\nmuitas tentativas, aguarde 15 minutos");
  }

  const h = await hashPass(env, rec.salt, pass);

  if (!safeEqual(h, rec.hash)) {
    try {
      await env.DB.put(
        "fail:" + user,
        String(fails + 1),
        { expirationTtl: LOCK_SECONDS }
      );
    } catch (e) {}

    await logEvent(env, user, "falha", request);

    return text(401, "ERRO\nusuario ou senha invalidos");
  }

  if (fails > 0) {
    try {
      await env.DB.delete("fail:" + user);
    } catch (e) {}
  }

  const st = accountState(rec);

  if (st !== "ativo" && st !== "pendente" && st !== "vencido") {
    return text(401, "ERRO\nusuario ou senha invalidos");
  }

  // Trava por console: a conta fica presa ao primeiro console que entrar.
  let changed = false;
  if (dev) {
    const dh = await hashDev(env, dev);
    if (rec.dev && rec.dev !== dh) {
      await logEvent(env, user, "outro-console", request);
      return text(
        403,
        "ERRO\nEsta conta já está em uso em outro PS4. Peça a troca pelo WhatsApp."
      );
    }
    if (!rec.dev) { rec.dev = dh; changed = true; }
  }

  // Conta pendente ou vencida entra so para ver o catalogo (sem downloads).
  if (isLocked(st)) {
    await logEvent(env, user, st, request);

    if (changed) {
      try { await putUser(env, user, rec, (await userMeta(env, user)).last); } catch (e) {}
    }

    const { token, exp } = await makeToken(env, user);

    return text(
      200,
      "OK\n" + token + "\n" + exp + "\n" + planLine(rec, st) + "\nLOCKED"
    );
  }

  if (changed || !recentSameIp(rec, request)) {
    if (!recentSameIp(rec, request)) {
      await logEvent(env, user, "ok", request);
      trackIp(rec, request);
    }

    try {
      await putUser(
        env,
        user,
        rec,
        new Date(Date.now()).toISOString()
      );
    } catch (e) {
      // O login continua funcionando mesmo se o KV atingir a cota.
    }
  }

  const { token, exp } = await makeToken(env, user);

  return text(200, "OK\n" + token + "\n" + exp + "\n" + planLine(rec, st));
}

// CADASTRO
async function register(request, env) {
  const f = await readForm(request);

  const user = String(f.user || "").trim().toLowerCase();
  const pass = String(f.pass || "");
  const dev = cleanDev(f.dev);

  if (!/^[a-z0-9_.-]{2,32}$/.test(user)) {
    return text(
      400,
      "ERRO\nUsuário: 2 a 32 letras minúsculas, números, . _ -"
    );
  }

  if (pass.length < 8 || pass.length > 128) {
    return text(
      400,
      "ERRO\nA senha precisa ter de 8 a 128 caracteres."
    );
  }

  const ip = request.headers.get("CF-Connecting-IP") || "?";
  const rk = "reg:" + ip;

  const tries = parseInt(
    (await env.DB.get(rk)) || "0",
    10
  );

  if (tries >= REG_MAX_PER_IP) {
    return text(
      429,
      "ERRO\nEste endereço de internet já fez cadastros. Se for o seu, fale com o admin."
    );
  }

  if (await env.DB.get("user:" + user)) {
    return text(
      409,
      "ERRO\nEsse usuário já existe. Escolha outro nome."
    );
  }

  // 1 cadastro por console: se este PS4 ja tem uma conta presa a ele, nao cria outra.
  if (dev) {
    const dh0 = await hashDev(env, dev);
    const dono = (await env.DB.get("regdev:" + dh0)) || (await env.DB.get("trial:" + dh0));
    if (dono && dono !== user) {
      const orec = await env.DB.get("user:" + dono, "json");
      if (orec && orec.dev === dh0) {
        return text(
          409,
          "ERRO\nEste console já tem um cadastro (usuário " + dono.slice(0, 2) + "***). Entre com ele ou fale com o admin."
        );
      }
    }
  }

  const pend = await env.DB.get("npending");
  const now = Date.now();

  const salt = b64u(
    crypto.getRandomValues(new Uint8Array(12))
  );

  const rec = {
    salt,
    hash: await hashPass(env, salt, pass),
    created: new Date(now).toISOString(),
    disabled: false,
    pending: false,
    expires: "",
    v2: 1,
    plan: ""
  };

  let estado = "pendente";
  let trialKey = "";

  if (dev) {
    const dh = await hashDev(env, dev);
    rec.dev = dh;
    trialKey = "trial:" + dh;

    if (!(await env.DB.get(trialKey))) {
      rec.plan = "trial";
      rec.expires = new Date(now + TRIAL_MS).toISOString();
      estado = "trial";
    } else {
      rec.plan = "";
      rec.expires = new Date(now - 1000).toISOString();   // este console ja usou o teste
      estado = "vencido";
    }
  } else {
    if (parseInt(pend || "0", 10) >= REG_MAX_PENDING) {
      return text(
        503,
        "ERRO\nCadastros temporariamente fechados."
      );
    }
    // App antigo (sem ID do console): nao ganha teste; o admin ativa pelo painel.
    rec.v2 = 0;
    rec.pending = true;
    rec.disabled = true;
  }

  try {
    await putUser(env, user, rec, "");
  } catch (e) {
    return text(
      503,
      "ERRO\nCadastros indisponíveis agora. Tente mais tarde."
    );
  }

  try {
    await env.DB.put(
      rk,
      String(tries + 1),
      { expirationTtl: REG_IP_TTL }
    );

    if (estado === "trial") {
      await env.DB.put(trialKey, user);
    }

    if (rec.dev) {
      await env.DB.put("regdev:" + rec.dev, user);
    }

    if (rec.pending) {
      await env.DB.put(
        "npending",
        String(parseInt(pend || "0", 10) + 1),
        { expirationTtl: 86400 }
      );
    }
  } catch (e) {}

  await logEvent(env, user, estado === "trial" ? "cadastro-trial" : "cadastro", request);

  return text(
    200,
    "OK\ncadastrado\n" + estado + "\n" + Math.floor((Date.parse(rec.expires) || 0) / 1000)
  );
}

function tokenFrom(request, url) {
  const auth = request.headers.get("Authorization") || "";

  return auth.startsWith("Bearer ")
    ? auth.slice(7)
    : url.searchParams.get("t") || "";
}

// DADOS DO PLANO
// Linhas: OK, usuario, plano|data, estado, agora(s), apoio(s), lembrete, vence(s), nome do plano
async function me(request, env, url) {
  const user = await readToken(
    env,
    tokenFrom(request, url)
  );

  if (!user) {
    return text(401, "ERRO\nsessao invalida");
  }

  const rec = await env.DB.get("user:" + user, "json");

  if (!rec) {
    return text(401, "ERRO\nsessao invalida");
  }

  const st = accountState(rec);
  const vence = rec.v2 && rec.expires ? Math.floor((Date.parse(rec.expires) || 0) / 1000) : 0;

  // App antigo mostra o lembrete de apoio se a data passar: contas v2 nunca disparam.
  const apoio = rec.v2
    ? (vence || 4102444800)
    : Math.floor(
        (
          Date.parse(rec.support_until || "") ||
          ((Date.parse(rec.created || "") || 0) + 30 * 86400000)
        ) / 1000
      );

  return text(
    200,
    "OK\n" +
      user +
      "\n" +
      planLine(rec, st) +
      "\n" +
      st +
      "\n" +
      Math.floor(Date.now() / 1000) +
      "\n" +
      apoio +
      "\n" +
      (!rec.v2 && rec.nag ? "1" : "0") +
      "\n" +
      vence +
      "\n" +
      (rec.v2 ? (rec.plan || (rec.expires ? "" : "vitalicio")) : "legado")
  );
}

// PLANOS (publico): o app monta a janela de assinatura e os QR Codes com estes dados.
function plans() {
  const out = ["OK", "WA\t" + WHATSAPP, "TRIAL\t" + Math.round(TRIAL_MS / 60000)];
  for (const id of Object.keys(PLANOS)) {
    const p = PLANOS[id];
    out.push(["PLAN", id, p.nome, p.preco, p.dias, p.pix].join("\t"));
  }
  return text(200, out.join("\n"));
}

// CATALOGO BLOQUEADO PARA QUEM AINDA NAO FOI ATIVADO.
function lockCatalog(body) {
  const out = ["LOCK\t1"];

  for (const line of String(body).split("\n")) {
    const l = line.replace(/\r$/, "");

    if (l.startsWith("BTN\t")) {
      const f = l.split("\t");
      const action = f[8] || "pkg";

      if (action === "pkg" || action === "download") {
        f[3] = "-";

        if (f.length > 9) f[9] = "";
        if (f.length > 10) f[10] = "";

        out.push(f.join("\t"));
        continue;
      }
    }

    out.push(l);
  }

  return out.join("\n");
}

// CATALOGO
async function catalog(request, env, url) {
  const user = await readToken(
    env,
    tokenFrom(request, url)
  );

  if (!user) {
    return text(401, "ERRO\nsessao invalida");
  }

  const rec = await env.DB.get("user:" + user, "json");

  if (!rec) {
    return text(401, "ERRO\nsessao invalida");
  }

  const st = accountState(rec);

  if (st !== "ativo" && !isLocked(st)) {
    return text(401, "ERRO\nsessao invalida");
  }

  const body = await env.DB.get("catalog");

  if (!body) {
    return text(404, "ERRO\ncatalogo vazio");
  }

  const appAntigo = EXIGIR_APP_NOVO && !request.headers.get("X-JK-App");

  return text(
    200,
    st === "ativo" && !appAntigo ? body : lockCatalog(body)
  );
}

// PREVIEW PUBLICO
async function preview(env) {
  const body = await env.DB.get("catalog");

  if (!body) {
    return text(404, "ERRO\ncatalogo vazio");
  }

  return text(200, lockCatalog(body));
}

// ---------- ADMINISTRACAO ----------
async function listUsers(env) {
  const out = [];
  let cursor;
  do {
    const l = await env.DB.list({ prefix: "user:", cursor });
    for (const k of l.keys) out.push(k);
    cursor = l.list_complete ? undefined : l.cursor;
  } while (cursor);
  return out;
}

// Regra da migracao das contas antigas (sem "v2").
//  - com lembrete fixo (nao pagou): vence em 24 h
//  - sem lembrete (pago): mantem o prazo do apoio; se ja passou, ganha 30 dias a partir de hoje (revisar)
function migrationPlan(m, now) {
  const created = Date.parse(m.created || "") || 0;
  const sup = Date.parse(m.sup || "");
  if (m.nag) {
    return { regra: "lembrete", expires: now + DAY_MS };
  }
  const base = sup || (created ? created + 30 * DAY_MS : 0);
  if (base && base > now) {
    return { regra: "pago", expires: base };
  }
  return { regra: "pago-vencido", expires: now + 30 * DAY_MS };
}

async function admin(request, env, url) {
  const viaPanel = await panelTokenOk(env, request.headers.get("X-Panel-Token") || "");
  if (
    !viaPanel &&
    !safeEqual(
      request.headers.get("X-Admin-Key") || "",
      env.ADMIN_KEY
    )
  ) {
    return json(401, {
      erro: "chave de admin invalida"
    });
  }

  const p = url.pathname.replace("/admin/api/", "");
  const m = request.method;

  // LISTAR USUARIOS
  if (p === "users" && m === "GET") {
    const keys = await listUsers(env);

    return json(
      200,
      keys.map((k) => {
        const m = k.metadata || {};

        const st = accountState({
          disabled: m.disabled,
          pending: m.pending,
          expires: m.expires,
          v2: m.v2
        });

        return {
          user: k.name.slice(5),
          state: st,
          disabled: !!m.disabled,
          created: m.created || "",
          expires: m.expires || "",
          v2: !!m.v2,
          plan: m.plan || "",
          dev: !!m.dev,
          last: m.last || "",
          sup: m.sup || "",
          nag: !!m.nag,
          ips: (m.ips || []).map((x) => ({
            ip: x[0],
            t: x[1],
            c: x[2]
          }))
        };
      })
    );
  }

  // BACKUP COMPLETO (inclui hashes de senha: guarde em local seguro)
  if (p === "export" && m === "GET") {
    const keys = await listUsers(env);
    const out = {};
    for (const k of keys.slice(0, 400)) {
      out[k.name.slice(5)] = await env.DB.get(k.name, "json");
    }
    return json(200, { total: keys.length, parcial: keys.length > 400, usuarios: out });
  }

  // CRIAR USUARIO OU REDEFINIR SENHA
  if (p === "users" && m === "POST") {
    const f = await request.json();

    const user = String(f.user || "")
      .trim()
      .toLowerCase();

    const pass = String(f.pass || "");

    if (!/^[a-z0-9_.-]{2,32}$/.test(user)) {
      return json(400, {
        erro: "usuario: 2 a 32 letras minusculas, numeros, . _ -"
      });
    }

    if (pass.length < 8) {
      return json(400, {
        erro: "senha com no minimo 8 caracteres"
      });
    }

    const salt = b64u(
      crypto.getRandomValues(new Uint8Array(12))
    );

    const old = await env.DB.get(
      "user:" + user,
      "json"
    );

    // Usuario novo criado pelo admin nasce ativo e vitalicio (sem vencimento).
    // Redefinir senha mantem o estado atual da conta.
    const rec = old
      ? {
          ...old,
          salt,
          hash: await hashPass(env, salt, pass)
        }
      : {
          salt,
          hash: await hashPass(env, salt, pass),
          created: new Date(Date.now()).toISOString(),
          disabled: false,
          pending: false,
          expires: "",
          v2: 1,
          plan: "vitalicio"
        };

    await putUser(
      env,
      user,
      rec,
      (await userMeta(env, user)).last
    );

    await env.DB.delete("fail:" + user);

    return json(200, { ok: true });
  }

  // DESATIVAR / REATIVAR
  if (p === "users/disable" && m === "POST") {
    const f = await request.json();

    const rec = await env.DB.get(
      "user:" + f.user,
      "json"
    );

    if (!rec) {
      return json(404, {
        erro: "usuario nao existe"
      });
    }

    rec.disabled = !!f.disabled;

    if (rec.disabled) {
      rec.pending = false;
    }

    await putUser(
      env,
      f.user,
      rec,
      (await userMeta(env, f.user)).last
    );

    return json(200, { ok: true });
  }

  // PLANO: soma dias ao prazo atual (renovar antes de vencer nao perde dias).
  // plan: mensal | semestral | anual | vitalicio | vencer | dias (com "dias": N)
  if (p === "users/plan" && m === "POST") {
    const f = await request.json();

    const rec = await env.DB.get(
      "user:" + f.user,
      "json"
    );

    if (!rec) {
      return json(404, {
        erro: "usuario nao existe"
      });
    }

    const now = Date.now();
    const plan = String(f.plan || "");

    if (rec.pending) {
      const atual = parseInt(
        (await env.DB.get("npending")) || "0",
        10
      );

      try {
        await env.DB.put(
          "npending",
          String(Math.max(0, atual - 1)),
          { expirationTtl: 86400 }
        );
      } catch (e) {}
    }

    let dias = 0;
    let addMs = 0;
    const atualExp = rec.v2 && rec.expires ? Date.parse(rec.expires) || 0 : 0;

    if (plan === "teste1h" || plan === "teste24h") {
      addMs = plan === "teste1h" ? HOUR_MS : 24 * HOUR_MS;
    } else if (PLANOS[plan]) {
      dias = PLANOS[plan].dias;
    } else if (plan === "dias") {
      dias = Math.floor(Number(f.dias) || 0);
      if (dias < 1 || dias > 3650) {
        return json(400, { erro: "dias invalido (1 a 3650)" });
      }
    } else if (plan === "vitalicio") {
      rec.expires = "";
    } else if (plan === "vencer") {
      rec.expires = new Date(now - 1000).toISOString();
    } else {
      return json(400, { erro: "plano invalido" });
    }

    if (addMs) {
      rec.expires = new Date(Math.max(now, atualExp) + addMs).toISOString();
    }

    if (dias) {
      const atual = rec.v2 && rec.expires ? Date.parse(rec.expires) || 0 : 0;
      rec.expires = new Date(Math.max(now, atual) + dias * DAY_MS).toISOString();
    }

    rec.v2 = 1;
    rec.pending = false;
    rec.disabled = false;
    rec.plan = addMs ? ((rec.plan && rec.plan !== "trial" && atualExp > now) ? rec.plan : "trial") : PLANOS[plan] ? plan : (plan === "dias" ? (rec.plan && rec.plan !== "trial" ? rec.plan : "mensal") : plan === "vitalicio" ? "vitalicio" : "");
    rec.nag = false;
    rec.support_until = rec.expires || "";

    await putUser(
      env,
      f.user,
      rec,
      (await userMeta(env, f.user)).last
    );

    return json(200, {
      ok: true,
      expires: rec.expires || "",
      plan: rec.plan,
      state: accountState(rec)
    });
  }

  // LIBERAR CONSOLE (troca de PS4): a conta prende ao proximo console que entrar
  if (p === "users/device" && m === "POST") {
    const f = await request.json();

    const rec = await env.DB.get(
      "user:" + f.user,
      "json"
    );

    if (!rec) {
      return json(404, {
        erro: "usuario nao existe"
      });
    }

    delete rec.dev;

    await putUser(
      env,
      f.user,
      rec,
      (await userMeta(env, f.user)).last
    );

    return json(200, { ok: true });
  }

  // EXCLUIR USUARIO
  if (p === "users/delete" && m === "POST") {
    const f = await request.json();

    await env.DB.delete("user:" + f.user);

    return json(200, { ok: true });
  }

  // MIGRACAO DAS CONTAS ANTIGAS PARA OS PLANOS
  // GET migrate = previa (nao grava nada). POST migrate {limit} aplica em lotes.
  if (p === "migrate" && m === "GET") {
    const now = Date.now();
    const keys = await listUsers(env);
    const itens = [];
    const cont = { lembrete: 0, pago: 0, "pago-vencido": 0, pendente: 0, desativado: 0, "ja-migrado": 0 };

    for (const k of keys) {
      let md = k.metadata || {};
      if (!md.created && !md.v2 && md.disabled === undefined) {
        const r = await env.DB.get(k.name, "json");
        if (r) md = { disabled: !!r.disabled, pending: !!r.pending, created: r.created, sup: r.support_until, nag: !!r.nag, v2: r.v2 };
      }
      if (md.v2) { cont["ja-migrado"]++; continue; }
      if (md.pending) { cont.pendente++; continue; }
      if (md.disabled) { cont.desativado++; continue; }
      const pl = migrationPlan(md, now);
      cont[pl.regra]++;
      itens.push({
        user: k.name.slice(5),
        regra: pl.regra,
        nag: !!md.nag,
        sup: md.sup || "",
        cadastro: md.created || "",
        vence: new Date(pl.expires).toISOString()
      });
    }

    return json(200, { total: keys.length, contagem: cont, itens });
  }

  if (p === "migrate" && m === "POST") {
    const f = await request.json().catch(() => ({}));
    if (f.confirmar !== "APLICAR") {
      return json(400, { erro: "confirmacao ausente" });
    }
    const lim = Math.min(Math.max(parseInt(f.limit || "120", 10) || 120, 1), 200);
    const now = Date.now();
    const keys = await listUsers(env);
    let feitos = 0, pulados = 0, restantes = 0, erro = "";

    for (const k of keys) {
      const md = k.metadata || {};
      if (md.v2 || md.pending || md.disabled) { pulados++; continue; }
      if (feitos >= lim) { restantes++; continue; }
      try {
        const rec = await env.DB.get(k.name, "json");
        if (!rec || rec.v2 || rec.pending || rec.disabled) { pulados++; continue; }
        const pl = migrationPlan(
          { created: rec.created, sup: rec.support_until, nag: rec.nag },
          now
        );
        rec.mig_prev = { expires: rec.expires || "", nag: !!rec.nag, sup: rec.support_until || "" };
        rec.v2 = 1;
        rec.plan = "legado";
        rec.expires = new Date(pl.expires).toISOString();
        rec.nag = false;
        rec.support_until = rec.expires;
        await putUser(env, k.name.slice(5), rec, md.last || "");
        feitos++;
      } catch (e) {
        erro = String((e && e.message) || e);
        restantes++;
        break;
      }
    }

    return json(200, { ok: !erro, feitos, pulados, restantes, erro });
  }

  // DESFAZER A MIGRACAO (volta o campo antigo; contas deixam de ter vencimento)
  if (p === "migrate/undo" && m === "POST") {
    const f = await request.json().catch(() => ({}));
    if (f.confirmar !== "DESFAZER") {
      return json(400, { erro: "confirmacao ausente" });
    }
    const lim = Math.min(Math.max(parseInt(f.limit || "120", 10) || 120, 1), 200);
    const keys = await listUsers(env);
    let feitos = 0, restantes = 0, erro = "";

    for (const k of keys) {
      const md = k.metadata || {};
      if (!md.v2 || md.plan !== "legado") continue;
      if (feitos >= lim) { restantes++; continue; }
      try {
        const rec = await env.DB.get(k.name, "json");
        if (!rec || !rec.mig_prev) continue;
        rec.expires = rec.mig_prev.expires;
        rec.nag = rec.mig_prev.nag;
        rec.support_until = rec.mig_prev.sup;
        delete rec.mig_prev;
        delete rec.v2;
        delete rec.plan;
        await putUser(env, k.name.slice(5), rec, md.last || "");
        feitos++;
      } catch (e) {
        erro = String((e && e.message) || e);
        restantes++;
        break;
      }
    }

    return json(200, { ok: !erro, feitos, restantes, erro });
  }

  // HISTORICO DE ACESSOS
  if (p === "logins" && m === "GET") {
    const lim = Math.min(
      Math.max(
        parseInt(url.searchParams.get("limit") || "10", 10) || 10,
        1
      ),
      100
    );

    const l = await env.DB.list({
      prefix: "log:",
      limit: lim,
      cursor: url.searchParams.get("cursor") || undefined
    });

    return json(200, {
      items: l.keys
        .map((k) => k.metadata)
        .filter(Boolean),
      cursor: l.list_complete ? "" : l.cursor
    });
  }

  // LER CATALOGO
  if (p === "catalog" && m === "GET") {
    return json(200, {
      catalog: (await env.DB.get("catalog")) || ""
    });
  }

  // SALVAR CATALOGO
  if (p === "catalog" && m === "PUT") {
    const f = await request.json();

    await env.DB.put(
      "catalog",
      String(f.catalog || "")
    );

    return json(200, { ok: true });
  }

  return json(404, {
    erro: "rota inexistente"
  });
}

// ---------- PAINEL ADMIN ----------
const ADMIN_HTML = `<!DOCTYPE html><html lang="pt-BR"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">

<title>Admin - PS4 Patch Store</title><style>

body{margin:0;background:#000000;color:#f4f7ff;font:14px system-ui,sans-serif;padding:20px;max-width:1600px;margin:auto}

h1{color:#42e6a4;font-size:22px}h2{margin-top:28px;font-size:16px;color:#42e6a4}

input,textarea,button,select{font:inherit;padding:8px 10px;border-radius:6px;border:1px solid #2b2f3a;background:#0d0e12;color:#f4f7ff}

button{background:#42e6a4;color:#06140f;border:0;font-weight:700;cursor:pointer;margin:2px}button.sec{background:#17191f;color:#f4f7ff;border:1px solid #2b2f3a}button.pl{background:#6fb4ff;color:#06140f}button:disabled{opacity:.5;cursor:wait}

table{width:100%;border-collapse:collapse;margin-top:8px}td,th{padding:7px;border-bottom:1px solid #23262f;text-align:left;vertical-align:top}th{color:#a1aec4;font-weight:600}

.ok{color:#42e6a4}.warn{color:#ffc857;font-weight:700}.bad{color:#ff7a7a}.mut{color:#6b7a94}.box{background:#0d0e12;border:1px solid #23262f;border-radius:10px;padding:14px;margin-top:10px}code{background:#17191f;padding:2px 6px;border-radius:4px}

.modal{position:fixed;inset:0;background:rgba(0,0,0,.65);display:flex;align-items:center;justify-content:center;padding:16px}.modal[hidden]{display:none}

.mbox{background:#0d0e12;border:1px solid #2b2f3a;border-radius:12px;width:100%;max-width:900px;max-height:80vh;display:flex;flex-direction:column}

.mhead{display:flex;justify-content:space-between;align-items:center;padding:12px 16px;border-bottom:1px solid #23262f;color:#42e6a4;font-weight:700}

.mbody{overflow:auto;padding:0 16px 12px}

.ipl{font-size:12px;color:#a1aec4;white-space:nowrap}td.act{white-space:nowrap}tr.pend td{background:rgba(255,200,87,.08)}tr.venc td{background:rgba(255,122,122,.06)}

.kp{display:inline-block;vertical-align:top;width:23.4%;margin:0 1.5% 10px 0;box-sizing:border-box;background:#0d0e12;border:1px solid #23262f;border-top:3px solid #23262f;border-radius:10px;padding:12px 16px;font-size:14px}.kp b{display:block;font-size:34px;line-height:1.05}.kp span{color:#a1aec4;font-size:12px;text-transform:uppercase;letter-spacing:1px}@media(max-width:700px){.kp{width:48%;margin-right:2%}}textarea{box-sizing:border-box}.row2{display:flex;gap:16px;align-items:stretch}.row2>div{flex:1 1 0;min-width:0;display:flex;flex-direction:column}.row2>div>.box{flex:1;display:flex;flex-direction:column}#cat{flex:1;min-height:250px}#chips button{border-radius:99px;margin:0 6px 6px 0}#chips button.on{background:#1b2f4a;border-color:#6fb4ff;color:#fff}#chips b{color:#6fb4ff}@media(max-width:900px){.row2{display:block}}</style></head><body><h1>PS4 Patch Store - Admin</h1>

<div class="box"><input id="key" type="password" placeholder="Chave de admin" style="width:240px"> <button onclick="entrar()">Entrar</button> <button id="btnAtualizar" class="sec" onclick="atualizar()" hidden>Atualizar</button> <span id="upd" style="font-size:12px;margin-left:6px"></span> <span id="msg"></span></div>

<div id="painel" hidden>

<div id="kpis" style="margin-top:14px;font-size:0"><div class="kp" style="border-top-color:#42e6a4"><b id="k1">0</b><span>Ativos</span></div><div class="kp" style="border-top-color:#6fb4ff"><b id="k2">0</b><span>Pendentes</span></div><div class="kp" style="border-top-color:#ffc857"><b id="k3">0</b><span>Vencem em 5 dias</span></div><div class="kp" style="border-top-color:#ff7a7a"><b id="k4">0</b><span>Vencidos</span></div></div>

<h2>Criar ou redefinir usuario</h2><div class="box">

<input id="nu" placeholder="usuario (ex.: joao)"> <input id="np" placeholder="senha"> <button class="sec" onclick="gerar()">Gerar senha</button> <button onclick="criar()">Salvar</button> <small class="mut">usuario novo criado aqui nasce vitalicio</small>

<div id="novo"></div></div>

<div class="row2"><div><h2>Catalogo do app (opcional)</h2><div class="box"><textarea id="cat" rows="7" style="width:100%" placeholder="conteudo do catalog.dat"></textarea><br><button onclick="salvarCat()" style="margin-top:8px;align-self:flex-start">Salvar catalogo</button></div>

</div><div><h2>Ultimos acessos</h2><div class="box"><table id="lrec" style="margin:0"></table><button class="sec" onclick="abrirLogs()" style="margin-top:8px">Ver historico completo</button></div>

</div></div>

<h2>Usuarios <span id="pcount"></span> <small class="mut" style="font-weight:400">(usuario escreveu "assinei o plano X" no WhatsApp: ache o nome aqui e clique no plano; os dias somam ao prazo atual)</small></h2>
<div id="chips" style="margin:10px 0"><button class="sec on" data-f="">Todos <b id="c0"></b></button><button class="sec" data-f="pendente">Pendentes <b id="c1"></b></button><button class="sec" data-f="vencendo">Vencendo (5 dias) <b id="c2"></b></button><button class="sec" data-f="vencido">Vencidos <b id="c3"></b></button><button class="sec" data-f="ativo">Ativos <b id="c4"></b></button><button class="sec" data-f="trial">Teste 1h <b id="c5"></b></button><button class="sec" data-f="ips">⚠ Varios IPs <b id="c6"></b></button></div>
<div><input id="busca" placeholder="buscar usuario" oninput="desenhar()"> <select id="filtro" onchange="desenhar()" style="display:none"><option value="">todos</option><option value="ativo">ativos</option><option value="vencido">vencidos</option><option value="pendente">pendentes</option><option value="desativado">desativados</option><option value="vencendo">vencendo</option><option value="ips">varios IPs</option><option value="trial">teste 1h</option><option value="legado">legado / sem prazo</option></select> <span id="qtd" class="mut"></span></div>
<table id="users"></table>

<h2>Migracao das contas antigas para os planos</h2><div class="box">
<div class="mut" style="margin-bottom:8px">Contas antigas continuam sem vencimento ate voce aplicar. Regra: <b>com lembrete fixo</b> vence em 24 h; <b>sem lembrete</b> (pago) mantem o prazo do apoio (se ja passou, ganha 30 dias a partir de hoje).</div>
<button class="sec" onclick="backup()">1. Baixar backup</button> <button class="sec" onclick="previa()">2. Ver previa</button> <button onclick="aplicarMig()">3. Aplicar migracao</button> <button class="sec" onclick="desfazerMig()">Desfazer migracao</button>
<div id="mig" style="margin-top:10px"></div></div>

</div>

<div id="modal" class="modal" hidden onclick="if(event.target===this)fecharLogs()"><div class="mbox">

<div class="mhead"><span>Historico de acessos <small id="lupd" style="color:#a1aec4;font-weight:400"></small></span><span><button class="sec" onclick="abrirLogs()">Atualizar</button><button class="sec" onclick="fecharLogs()">Fechar</button></span></div>

<div class="mbody" id="mbody"><table id="logs"></table><div style="text-align:center;margin-top:8px"><button class="sec" id="lmais" onclick="maisLogs()">Carregar mais</button></div></div>

</div></div><script>

const $=id=>document.getElementById(id);let KEY="";let PTK="";

async function api(p,m,b){const r=await fetch("/admin/api/"+p,{method:m||"GET",headers:{"X-Admin-Key":KEY,"X-Panel-Token":PTK,"Content-Type":"application/json"},body:b?JSON.stringify(b):undefined});const j=await r.json();if(!r.ok)throw new Error(j.erro||r.status);return j}

function esc(s){return String(s).replace(/[&<>"]/g,c=>({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;"}[c]))}

function fmt(t){return t?new Date(t).toLocaleString("pt-BR"):"nunca"}

function dtc(t){return t?new Date(t).toLocaleString("pt-BR",{day:"2-digit",month:"2-digit",year:"numeric",hour:"2-digit",minute:"2-digit"}):"-"}

function dur(ms){const a=Math.abs(ms),m=Math.floor(a/6e4),h=Math.floor(a/36e5),d=Math.floor(a/864e5);return d>=2?d+" dias":h>=1?h+" h":m+" min"}

function venc(u){
 if(u.state==="pendente"||u.state==="desativado")return "<span class='mut'>-</span>";
 if(!u.v2)return "<span class='mut'>legado (sem prazo)</span>"+(u.nag?" <small class='warn'>(lembrete fixo)</small>":"");
 if(!u.expires)return "<span class='ok'>vitalicio</span>";
 const ms=Date.parse(u.expires)-Date.now();
 const dt=dtc(u.expires);
 if(ms<=0)return "<span class='bad'>vencido ha "+dur(ms)+"</span><div class='ipl'>"+dt+"</div>";
 return "<span class='"+(ms<=48*36e5?"warn":"ok")+"'>faltam "+dur(ms)+"</span><div class='ipl'>"+dt+"</div>";
}

function planoNome(u){if(u.state==="pendente")return "<span class='mut'>-</span>";if(!u.v2)return "<span class='mut'>legado</span>";return esc(u.plan||"-")}

let US=[],BUSY={},ORD="venc";

function cad(u){return Date.parse(u.created)||0}

function cmpCad(a,b){const pa=a.state==="pendente",pb=b.state==="pendente";if(pa!==pb)return pa?-1:1;if(pa)return cad(b)-cad(a);const d=cad(a)-cad(b);return d||String(a.user).localeCompare(String(b.user))}

function ex(u){return u.v2&&u.expires?Date.parse(u.expires):(u.v2?9e15:8e15)}

function cmp(a,b){if(ORD==="cad")return cmpCad(a,b);const pa=a.state==="pendente",pb=b.state==="pendente";if(pa!==pb)return pa?-1:1;const d=ex(a)-ex(b);return d||String(a.user).localeCompare(String(b.user))}

function ipcell(u){const a=u.ips||[];if(!a.length)return "<span class='mut'>-</span>";const lim=Date.now()-7*864e5;const n=a.filter(x=>Date.parse(x.t)>lim).length;const h=n>=2?"<div class='warn'>⚠ "+n+" IPs em 7 dias</div>":"";const f=x=>"<div class='ipl'>"+esc(x.ip)+" "+esc(x.c||"")+" · "+new Date(x.t).toLocaleString("pt-BR",{day:"2-digit",month:"2-digit",hour:"2-digit",minute:"2-digit"})+"</div>";return h+a.slice(0,3).map(f).join("")+(a.length>3?"<div class='ipl'>+"+(a.length-3)+" mais antigo"+(a.length>4?"s":"")+"</div>":"")}

function passa(u){const q=($("busca").value||"").trim().toLowerCase();if(q&&u.user.indexOf(q)<0)return false;const f=$("filtro").value;if(!f)return true;if(f==="trial")return u.plan==="trial";if(f==="legado")return !u.v2&&u.state==="ativo";if(f==="vencendo"){if(u.state!=="ativo"||!u.v2||!u.expires)return false;var m=Date.parse(u.expires)-Date.now();return m>0&&m<=5*864e5}if(f==="ips"){var lim=Date.now()-7*864e5;return (u.ips||[]).filter(function(i){return Date.parse(i.t)>lim}).length>=2}return u.state===f}

 function kpis(){var a=0,p=0,v=0,x=0,t=0,ip=0,n=Date.now(),lim=n-7*864e5;US.forEach(function(u){if(u.state==="ativo"){a++;if(u.v2&&u.expires){var m=Date.parse(u.expires)-n;if(m>0&&m<=5*864e5)v++}}else if(u.state==="pendente")p++;else if(u.state==="vencido")x++;if(u.plan==="trial")t++;if((u.ips||[]).filter(function(i){return Date.parse(i.t)>lim}).length>=2)ip++});$("k1").textContent=a;$("k2").textContent=p;$("k3").textContent=v;$("k4").textContent=x;$("c0").textContent=US.length;$("c1").textContent=p;$("c2").textContent=v;$("c3").textContent=x;$("c4").textContent=a;$("c5").textContent=t;$("c6").textContent=ip}
 function recentes(){api("logins?limit=8").then(function(r){$("lrec").innerHTML="<tr><th>Data</th><th>Usuario</th><th>Resultado</th><th>Pais / IP</th></tr>"+r.items.map(function(l){return "<tr><td>"+fmt(l.t)+"</td><td>"+esc(l.user)+"</td><td class='"+(l.result==="ok"?"ok":(l.result==="cadastro"||l.result==="cadastro-trial"||l.result==="pendente"||l.result==="vencido")?"warn":"bad")+"'>"+esc(l.result)+"</td><td>"+esc(l.country)+" "+esc(l.ip)+"</td></tr>"}).join("")}).catch(function(){})}
 document.addEventListener("click",function(e){var b=e.target.closest("#chips button");if(!b)return;$("filtro").value=b.getAttribute("data-f");var all=document.querySelectorAll("#chips button");for(var i=0;i<all.length;i++)all[i].className="sec"+(all[i]===b?" on":"");desenhar()});
function desenhar(){
 kpis();

 const L=US.filter(passa).sort(cmp);

 const np=US.filter(u=>u.state==="pendente").length,nv=US.filter(u=>u.state==="vencido").length;$("pcount").innerHTML=(np?"<b class='warn'>("+np+" pendente"+(np>1?"s":"")+")</b> ":"")+(nv?"<b class='bad'>("+nv+" vencido"+(nv>1?"s":"")+")</b>":"");$("qtd").textContent=L.length+" de "+US.length;

 $("users").innerHTML="<tr><th>#</th><th>Usuario</th><th data-s='cad' style='cursor:pointer'>Cadastro"+(ORD==="cad"?" ▼":"")+"</th><th>Plano</th><th data-s='venc' style='cursor:pointer'>Vence"+(ORD==="venc"?" ▲":"")+"</th><th>Ultimo login</th><th>IPs de login</th><th>Console</th><th>Estado</th><th></th></tr>"+L.map((u,ix)=>{const st=u.state,on=st==="ativo",pend=st==="pendente",bz=BUSY[u.user]?" disabled":"",q=esc(u.user);

  const bt=(a,l,cls,p)=>"<button class='"+(cls||"")+"' data-a='"+a+"' data-u='"+q+"'"+(p?" data-p='"+p+"'":"")+bz+">"+l+"</button>";

  const planos=st==="desativado"?bt("reativar","Reativar",""):bt("plan","+ Teste 1h","sec","teste1h")+bt("plan","+ Teste 24h","sec","teste24h")+bt("plan","+ Mensal","pl","mensal")+bt("plan","+ Semestral","pl","semestral")+bt("plan","+ Anual","pl","anual")+bt("plan","Vitalicio","sec","vitalicio")+(on||st==="vencido"?bt("desat","Desativar","sec"):"");

  return "<tr class='"+(pend?"pend":st==="vencido"?"venc":"")+"'><td class='mut'>"+(ix+1)+"</td><td>"+q+"</td><td>"+dtc(u.created)+"</td><td>"+planoNome(u)+"</td><td>"+venc(u)+"</td><td>"+fmt(u.last)+"</td><td>"+ipcell(u)+"</td><td>"+(u.dev?"<span class='ok'>preso</span> "+bt("liberar","Liberar","sec"):"<span class='mut'>-</span>")+"</td><td class='"+(on?"ok":pend?"warn":"bad")+"'>"+esc(st)+"</td><td class='act'>"+planos+bt("nova","Nova senha","sec")+bt("apagar","Excluir","sec")+"</td></tr>"}).join("")}

async function acao(u,fn){if(BUSY[u])return;BUSY[u]=1;desenhar();try{await fn()}catch(e){alert(e.message)}BUSY[u]=0;desenhar()}

async function darPlano(u,p){await acao(u,async()=>{const r=await api("users/plan","POST",{user:u,plan:p});const x=US.find(a=>a.user===u);if(x){x.expires=r.expires;x.plan=r.plan;x.state=r.state;x.v2=true;x.disabled=false;x.nag=false}})}

async function liberar(u){if(!confirm("Liberar o console de "+u+"? A conta prende ao proximo PS4 que entrar."))return;await acao(u,async()=>{await api("users/device","POST",{user:u});const x=US.find(a=>a.user===u);if(x)x.dev=false})}

async function reativar(u){await acao(u,async()=>{await api("users/disable","POST",{user:u,disabled:false});const x=US.find(a=>a.user===u);if(x){x.disabled=false;x.state="ativo"}})}

async function entrar(){KEY=$("key").value;try{await api("users");$("painel").hidden=false;$("btnAtualizar").hidden=false;$("msg").textContent="";carregar()}catch(e){$("msg").textContent=e.message;$("msg").className="bad"}}

document.addEventListener("click",e=>{const h=e.target.closest("th[data-s]");if(h){ORD=h.dataset.s;desenhar();return}const b=e.target.closest("button[data-a]");if(!b)return;const u=b.dataset.u,a=b.dataset.a;if(a==="plan")darPlano(u,b.dataset.p);else if(a==="liberar")liberar(u);else if(a==="reativar")reativar(u);else if(a==="desat")desat(u,true);else if(a==="nova")nova(u);else if(a==="apagar")apagar(u)});

async function atualizar(){const b=$("btnAtualizar");b.disabled=true;b.textContent="Atualizando...";try{US=await api("users");desenhar();recentes();if(!$("modal").hidden)abrirLogs();$("upd").textContent="Atualizado "+new Date().toLocaleTimeString("pt-BR");$("upd").className="ok"}catch(e){$("upd").textContent=e.message;$("upd").className="bad"}b.disabled=false;b.textContent="Atualizar"}

async function carregar(){

 US=await api("users");desenhar();

 $("cat").value=(await api("catalog")).catalog;recentes();

}

async function backup(){try{const r=await api("export");const a=document.createElement("a");a.href=URL.createObjectURL(new Blob([JSON.stringify(r,null,1)],{type:"application/json"}));a.download="backup_usuarios_"+new Date().toISOString().slice(0,10)+".json";a.click();$("mig").innerHTML="<span class='ok'>Backup baixado ("+r.total+" usuarios).</span> Guarde o arquivo: ele tem os hashes das senhas."+(r.parcial?" <b class='bad'>Parcial: so os primeiros 400.</b>":"")}catch(e){$("mig").innerHTML="<span class='bad'>"+esc(e.message)+"</span>"}}

async function previa(){try{const r=await api("migrate");const c=r.contagem;let h="<div>Total de contas: <b>"+r.total+"</b> &middot; ja migradas: "+c["ja-migrado"]+" &middot; pendentes (nao mudam): "+c.pendente+" &middot; desativadas (nao mudam): "+c.desativado+"</div><div style='margin:6px 0'><b class='warn'>"+c.lembrete+"</b> com lembrete fixo (vencem em 24 h) &middot; <b class='ok'>"+c.pago+"</b> pagos mantem o prazo &middot; <b class='bad'>"+c["pago-vencido"]+"</b> sem lembrete mas com apoio ja vencido (+30 dias a partir de hoje, revisar)</div>";h+="<table><tr><th>Usuario</th><th>Regra</th><th>Cadastro</th><th>Apoio ate</th><th>Passa a vencer em</th></tr>"+r.itens.sort((a,b)=>a.vence<b.vence?-1:1).map(i=>"<tr><td>"+esc(i.user)+"</td><td class='"+(i.regra==="lembrete"?"warn":i.regra==="pago"?"ok":"bad")+"'>"+esc(i.regra)+"</td><td>"+dtc(i.cadastro)+"</td><td>"+dtc(i.sup)+"</td><td>"+dtc(i.vence)+"</td></tr>").join("")+"</table>";$("mig").innerHTML=h}catch(e){$("mig").innerHTML="<span class='bad'>"+esc(e.message)+"</span>"}}

async function lotes(rota,conf,rotulo){const el=$("mig");let tot=0;for(let i=0;i<60;i++){const r=await api(rota,"POST",{confirmar:conf,limit:100});tot+=r.feitos;el.innerHTML="<span class='warn'>"+rotulo+": "+tot+" contas... (restam "+r.restantes+")</span>";if(r.erro){el.innerHTML="<span class='bad'>Parou: "+esc(r.erro)+" &mdash; "+tot+" contas feitas. Tente de novo mais tarde (continua de onde parou).</span>";return}if(!r.restantes)break}el.innerHTML="<span class='ok'>Concluido: "+tot+" contas.</span>";await atualizar()}

async function aplicarMig(){if(prompt("Isso passa as contas antigas para os planos e vai bloquear quem tem lembrete fixo em 24 h.\\nVoce baixou o backup e conferiu a previa?\\nDigite APLICAR para continuar:")!=="APLICAR")return;try{await lotes("migrate","APLICAR","Aplicando")}catch(e){$("mig").innerHTML="<span class='bad'>"+esc(e.message)+"</span>"}}

async function desfazerMig(){if(prompt("Volta as contas migradas para o estado antigo (sem vencimento). Digite DESFAZER:")!=="DESFAZER")return;try{await lotes("migrate/undo","DESFAZER","Desfazendo")}catch(e){$("mig").innerHTML="<span class='bad'>"+esc(e.message)+"</span>"}}

let LOGC="",LOGDONE=false,LOGBUSY=false,LOGG=0;

async function maisLogs(){if(LOGBUSY||LOGDONE)return;LOGBUSY=true;const g=LOGG,first=!LOGC;try{const r=await api("logins?limit=10"+(LOGC?"&cursor="+encodeURIComponent(LOGC):""));if(g!==LOGG)return;

 $("logs").insertAdjacentHTML("beforeend",r.items.map(l=>"<tr><td>"+fmt(l.t)+"</td><td>"+esc(l.user)+"</td><td class='"+(l.result==="ok"?"ok":(l.result==="cadastro"||l.result==="cadastro-trial"||l.result==="pendente"||l.result==="vencido")?"warn":"bad")+"'>"+esc(l.result)+"</td><td>"+esc(l.country)+" "+esc(l.ip)+"</td></tr>").join(""));

 LOGC=r.cursor||"";LOGDONE=!r.cursor;$("lmais").hidden=LOGDONE;if(first)$("lupd").textContent="atualizado "+new Date().toLocaleTimeString("pt-BR")}catch(e){alert(e.message)}LOGBUSY=false}

function abrirLogs(){LOGG++;LOGBUSY=false;$("modal").hidden=false;$("logs").innerHTML="<tr><th>Data</th><th>Usuario</th><th>Resultado</th><th>Pais / IP</th></tr>";LOGC="";LOGDONE=false;$("lmais").hidden=false;$("mbody").scrollTop=0;maisLogs()}

function fecharLogs(){$("modal").hidden=true}

document.addEventListener("keydown",e=>{if(e.key==="Escape")fecharLogs()});

document.getElementById("mbody").addEventListener("scroll",function(){if(this.scrollTop+this.clientHeight>=this.scrollHeight-24)maisLogs()});

function gerar(){const a="abcdefghjkmnpqrstuvwxyz23456789";const r=crypto.getRandomValues(new Uint8Array(10));$("np").value=[...r].map(x=>a[x%a.length]).join("")}

async function criar(){try{await api("users","POST",{user:$("nu").value,pass:$("np").value});$("novo").innerHTML="<p class='ok'>Salvo. Passe ao amigo: usuario <code>"+esc($("nu").value.toLowerCase())+"</code> senha <code>"+esc($("np").value)+"</code></p>";carregar()}catch(e){$("novo").innerHTML="<p class='bad'>"+esc(e.message)+"</p>"}}

function nova(u){$("nu").value=u;gerar();window.scrollTo(0,0)}

async function desat(u,d){await api("users/disable","POST",{user:u,disabled:d});const x=US.find(a=>a.user===u);if(x){x.state="desativado";x.disabled=true}desenhar()}

async function apagar(u){if(confirm("Excluir "+u+"?")){await api("users/delete","POST",{user:u});US=US.filter(a=>a.user!==u);desenhar()}}

async function salvarCat(){await api("catalog","PUT",{catalog:$("cat").value});alert("Catalogo salvo")}

try{if(window.parent!==window&&window.parent.TK){PTK=window.parent.TK;$("key").hidden=true;api("users").then(function(){$("painel").hidden=false;$("btnAtualizar").hidden=false;carregar()}).catch(function(e){$("msg").textContent=e.message;$("msg").className="bad"})}}catch(e){}

</script></body></html>`;

// ---------- INSTALADOR PELO NAVEGADOR DO PS4 (teste) ----------
// Payload DirectPackageInstaller (Marcus Sacana, GPL v3): https://github.com/marcussacana/DirectPackageInstaller
// O marcador B4 (6 bytes) guarda IP(4)+porta(2) do servidor de retorno; /p.bin troca esses bytes.
const DPI_PAYLOAD_B64 =
  "6YADAABmLg8fhAAAAAAAkPMPHvpVSInlSIHs0AEAAEiJvUj+//+JtUT+//9IiZU4/v//SImNMP7//8dF/AAAAABIjVX8SIuFSP7/" +
  "/7kAAAAAvgAAAABIicfojQEAAEiFwHQHuAAAAADrX0jHhVD+//+oAQAAi0X8SI2VUP7//74AAAAAicfocwEAAIXAdAe4AAAAAOsz" +
  "i0X0g/gBdyZIi4V4////SYnASIuNOP7//0iLhTD+//+6AAAAAEiJzkiJx0H/0ItF/EiYycPzDx76VUiJ5UiD7BBIiX34iXX0i3X0" +
  "SItF+LkAAAAAugAAAABIicfoGf///8nD8w8e+lVIieVIg+wgSIl96EiJdeBIx0X4AAAAAEiLReiJwUiNVfhIi0XgSInGic/oqwAA" +
  "AEiLRfjJw2YuDx+EAAAAAAAPHwBIx8ADAAAASYnKDwUPgr8AAADDSMfABAAAAEmJyg8FD4KsAAAAw0jHwAUAAABJicoPBQ+CmQAA" +
  "AMNIx8AGAAAASYnKDwUPgoYAAADDSMfAYQAAAEmJyg8FD4JzAAAAw0jHwGIAAABJicoPBQ+CYAAAAMNIx8CHAAAASYnKDwUPgk0A" +
  "AADDSMfA3QEAAEmJyg8FD4I6AAAAw0jHwE8CAABJicoPBQ+CJwAAAMNIx8BSAgAASYnKDwUPghQAAADDSMfAYAIAAEmJyg8FD4IB" +
  "AAAAw4kF+T0AAEgxwEj/yMO5AAgAAEgxwFDi/UiBxABAAADD8w8e+lVIieWJfexIiXXgSItF4MYAMEiLReBIg8ABxgB4x0X8BwAA" +
  "AMdF+AIAAADrU4tF/MHgAotV7InB0/qJ0IPgD4lF9IN99Al/GYtF9I1IMItF+Ehj0EiLReBIAdCJyogQ6xeLRfSNSDeLRfhIY9BI" +
  "i0XgSAHQicqIEINt/AGDRfgBg338AHmnSItF4EiDwArGAACQXcPzDx76VUiJ5UiJfehIiXXgSIlV2MdF/AAAAADHRfgAAAAA6yqL" +
  "RfhIY9BIi0XoSI0MAotF/I1QAYlV/Ehj0EiLRdhIAcIPtgGIAoNF+AGLRfhIY9BIi0XoSAHQD7YAhMB1wsdF9AAAAADrKotF9Ehj" +
  "0EiLReBIjQwCi0X8jVABiVX8SGPQSItF2EgBwg+2AYgCg0X0AYtF9Ehj0EiLReBIAdAPtgCEwHXCi0X8SGPQSItF2EgB0MYAAJBd" +
  "w/MPHvpVSInlSIHskAMAAEiNhTD///9IicfoLQ8AAEiNhTD///9IicfouBEAAEjHhWD///8AAAAASLgQAAAAAAAAOEiJhWj///9I" +
  "uAAAAAAAHABASImFcP///0i4AP8AAAAAAJBIiYV4////SI2FMP///0iJx+geEAAA6Br+//++AAAAAEiNPfAbAADotvz//0iJRfhI" +
  "i0X4SI01BBwAAEiJx+jN/P//SIlF8L4AAAAASI09HBwAAOiK/P//SIlF6EiLRehIjTUyHAAASInH6KH8//9IiUXgSItF6EiNNTkc" +
  "AABIicfoivz//0iJRdhIi0XoSI01QhwAAEiJx+hz/P//SIlF0L4AAAAASI09QhwAAOgw/P//SIlFyEiLRchIjTVRHAAASInH6Ef8" +
  "//9IiUXASItFyEiNNVccAABIicfoMPz//0iJRbhIi0XISI01aBwAAEiJx+gZ/P//SIlFsEiLRchIjTWBHAAASInH6AL8//9IiUWo" +
  "SItFyEiNNY0cAABIicfo6/v//0iJRaDoB/3//8eFLP///7wCAADHhSj///8AAAAASI2FLP///0iLVeBIicf/0kiNhSj///9Ii1XY" +
  "SInH/9JIi0XQ/9C+AAAAAEiNPUscAADoafv//0iJRZhIi0WYSI01YRwAAEiJx+iA+///SIlFkEiLRZD/0IlFjIN9jAAPhKkAAABI" +
  "x4WQ/P//AAAAAEjHhZj8//8AAAAASI2VoPz//7gAAAAAuT4AAABIidfzSKtIx4Vw/P//AAAAAEjHhXj8//8AAAAASMeFgPz//wAA" +
  "AABIx4WI/P//AAAAAEiNlXD8//+LRYxIidaJx+g0/P//SI2VkPz//0iNhXD8//9IicZIjT3WGwAA6K/8//9IjYWQ/P//SItV8EiJ" +
  "xr/eAAAA/9K4/////+nFAwAAQbkAAAAAQbj/////uQIQAAC6AwAAAL4AABAAvwAAAADoZ/v//0iJhRD///9Ix4UY////AAAQAEiN" +
  "hRD///9Ii1XASInH/9KJRYyDfYwAD4S2AAAAgX2MAQCZgA+EqQAAAEjHhZD8//8AAAAASMeFmPz//wAAAABIjZWg/P//uAAAAAC5" +
  "PgAAAEiJ1/NIq0jHhXD8//8AAAAASMeFePz//wAAAABIx4WA/P//AAAAAEjHhYj8//8AAAAASI2VcPz//4tFjEiJ1onH6Cr7//9I" +
  "jZWQ/P//SI2FcPz//0iJxkiNPeYaAADopfv//0iNhZD8//9Ii1XwSInGv94AAAD/0rj/////6bsCAABIjZWg/v//uAAAAAC5DQAA" +
  "AEiJ1/NIq4uFKP///4mFoP7//8eFpP7//wUAAABIjQWlGgAASImFqP7//0iNBZcaAABIiYWw/v//SI0FiRoAAEiJhcD+//9IjQV7" +
  "GgAASImFyP7//8eF2P7//wAAAQBIjQVkGgAASImF4P7//0iNBVgaAABIiYXw/v//SI0FRxoAAEiJhfj+//9IjYWg/v//SInH6BYF" +
  "AACJRYyDfYz/dRxIi0XwSI01JhoAAL/eAAAA/9C4/////+nuAQAAg32MAXUcSItF8EiNNRgaAAC/3gAAAP/QuAAAAADpzAEAAMeF" +
  "nP7///////9IjZWc/v//SI2FoP7//0iLTbhIidZIicf/0YlFjIF9jIgAmYB0IYuFnP7//4P4/3QWi4Wc/v//SItVqInH/9KJRYzp" +
  "dgEAAEiNlZz+//9IjYWg/v//SItNsEiJ1kiJx//RiUWMgX2MiACZgHQhi4Wc/v//g/j/dBaLhZz+//9Ii1Woicf/0olFjOkvAQAA" +
  "gX2MhgCZgHUXSItF8EiNNXoZAAC/3gAAAP/Q6Q8BAACBfYyIAJmAdRdIi0XwSI01uhkAAL/eAAAA/9Dp7wAAAIF9jDkAmYB0CYF9" +
  "jCYAo4B1F0iLRfBIjTWxGQAAv94AAAD/0OnGAAAAgX2MhQCZgHUXSItF8EiNNeEZAAC/3gAAAP/Q6aYAAABIx4WQ/P//AAAAAEjH" +
  "hZj8//8AAAAASI2VoPz//7gAAAAAuT4AAABIidfzSKtIx4Vw/P//AAAAAEjHhXj8//8AAAAASMeFgPz//wAAAABIx4WI/P//AAAA" +
  "AEiNlXD8//+LRYxIidaJx+hx+P//SI2VkPz//0iNhXD8//9IicZIjT2pGQAA6Oz4//9IjYWQ/P//SItV8EiJxr/eAAAA/9K4////" +
  "/+sF6d79///Jw/MPHvpVSInlSIPsIIl97EiNTfyLRey6BAAAAEiJzonH6Bv3//9IjUX8iwDJw/MPHvpVSInlSIPsIIl97EiNTfiL" +
  "Rey6CAAAAEiJzonH6O72//9IjUX4SIsAycPzDx76VUiJ5UiJfejHRfwAAAAA6wSDRfwBi0X8SGPQSItF6EgB0A+2AITAdeiLRfxd" +
  "w/MPHvpVSInlSIPsIIl97EiJdeCJVeiLReyJx+hT////iUX8SItF4MYAAItF6DlF/HYHuP/////rGYtV/EiLTeCLRexIic6Jx+hm" +
  "9v//uAAAAADJw/MPHvpVSInlSIPsMEiJfdhIiXXQSItF2EiJx+ha////iUX4i0X4SGPQSItF2EgB0EiJRfBIi0XQSInH6Dr///+J" +
  "RezHRfwAAAAA6yOLRfxIY9BIi0XQSAHQi1X8SGPKSItV8EgByg+2AIgCg0X8AYtF/DtF7HzVkJDJw/MPHvpVSInlSIPsIEiJfehI" +
  "iXXgSItF6MYAAEi4L3VzZXIvZGFIunRhL3RtcF8ASIlF8EiJVfhIjVXwSItF6EiJ1kiJx+g7////SItV4EiLRehIidZIicfoKP//" +
  "/5DJw/MPHvpVSInlSIHsABAAAEiDDCQASIPsUIm9zO///0iJtcDv//9IiZW47///SIuFwO///8YAAIuFzO///4nH6AH+//+JRfyD" +
  "ffwAdQq4AAAAAOntAAAASIuVuO///0iLhcDv//9IidZIicfoMf///0iLhcDv//+6/wEAAL4BBgAASInHuAAAAADoHfX//4lF5MdF" +
  "4AAQAABIx0Xw/////+tdi0X8SItV8CnQiUX8SI2F0O///0iJRejrN0iLVfBIi03oi0XkSInOicfoyPT//0iJRdhIg33YAH8HuAEA" +
  "AADrYkiLRdhIAUXoSItF2EgpRfBIg33wAH/Cg338AH41i0X8OUXgD05F4Ehj0EiNjdDv//+Lhczv//9Iic6Jx+hi9P//SIlF8EiD" +
  "ffAAD49w////6wGQi0Xkicfof/T//7gBAAAAycPzDx76VUiJ5UiB7AAQAABIgwwkAEiB7FADAABIib247P//SMeF8Pf//wAAAABI" +
  "x4X49///AAAAAEiNlQD4//+4AAAAALn+AAAASInX80irSMeFkPX//wAAAABIx4WY9f//AAAAAEiNlaD1//+4AAAAALlJAAAASInX" +
  "80irSIn6iAJIg8IBSMeFYPX//wAAAABIx4Vo9f//AAAAAEjHhXD1//8AAAAASMeFePX//wAAAABIx4WA9f//AAAAAEjHhYj1//8A" +
  "AAAASMeF8PT//wAAAABIx4X49P//AAAAAEjHhQD1//8AAAAASMeFCPX//wAAAABIx4UQ9f//AAAAAEjHhRj1//8AAAAASMeFIPX/" +
  "/wAAAABIx4Uo9f//AAAAAEjHhTD1//8AAAAASMeFOPX//wAAAABIx4VA9f//AAAAAEjHhUj1//8AAAAAx4VQ9f//AAAAAEjHhfDs" +
  "//8AAAAASMeF+Oz//wAAAABIjZUA7f//uAAAAAC5/gAAAEiJ1/NIq0jHheDs//8AAAAASMeF6Oz//wAAAABIx4XQ7P//AAAAAEjH" +
  "hdjs//8AAAAAxoXR7P//Ag+3BUcxAABmiYXS7P//iwU2MQAAiYXU7P//ugYAAAC+AQAAAL8CAAAA6Kjy//+JRfyDffwAeQq4////" +
  "/+nZAQAASI2N0Oz//4tF/LoQAAAASInOicfoj/L//4XAeRSLRfyJx+hb8v//uP/////pqAEAAItF/InH6M76//+FwHUUi0X8icfo" +
  "OfL//7gBAAAA6YYBAABIjY3w9///i0X8ugAIAABIic6Jx+gq+///g/j/dQq4/////+leAQAASI2NkPX//4tF/LpZAgAASInOicfo" +
  "Avv//4P4/3UKuP/////pNgEAAEiNjWD1//+LRfy6MAAAAEiJzonH6Nr6//+D+P91Crj/////6Q4BAABIjY3g7P//i0X8uhAAAABI" +
  "ic6Jx+iy+v//g/j/dQq4/////+nmAAAAi0X8icfoOfr//0iJRfDHhcvs//8ucG5nxoXP7P//AEiNlWD1//9IjYXw9P//SInWSInH" +
  "6L/6//9IjZXL7P//SI2F8PT//0iJ1kiJx+im+v//SI2V8PT//0iNjfDs//+LRfxIic6Jx+hm+///hcCLRfyJx+gX8f//SIuFuOz/" +
  "/0iNlWD1//9IiVAISIuFuOz//0iNlfD3//9IiVAQSIuFuOz//0iNlZD1//9IiVAgSIuFuOz//0iLVfBIiVBgSIuFuOz//0iNlfDs" +
  "//9IiVAoSIuFuOz//0iNleDs//9IiVBQuAAAAADJw/MPHvpVSInlSIPsQOiGBwAASIlF8EiLRfBIg8AIvgEAAABIicfoSAsAAEiJ" +
  "RfhIi0X4SI2wsAAAAEiNRcy5AQAAALoEAAAASInH6EgJAACFwA+FpgAAAItFzIP4AXVZSItF+EiDwEC+AQAAAEiJx+j+CgAASIlF" +
  "2EiLRfhIg8BIvgEAAABIicfo5QoAAEiJRdBIi0XYSI1wMEiNBaYuAAC5AQAAALoIAAAASInH6OUIAACFwHVT61hIi0X4vgEAAABI" +
  "icfoqQoAAEiJRehIi0XoSIPACL4BAAAASInH6JAKAABIiUXgSItF4Eg7Rfh1E0iLRehIiUX46TH///+Q6Qn///+Q6QP///+4////" +
  "/+s8SItF0EiNcBhIjQUvLgAAuQEAAAC6CAAAAEiJx+hmCAAAhcB0EkjHBQYuAAAAAAAAuP/////rBbgAAAAAycPzDx76VUiJ5UiL" +
  "BektAABIhcB1BeiZ/v//SIsF2C0AAF3D8w8e+lVIieVIiwXPLQAASIXAdQXod/7//0iLBb4tAABdw1VIieVIg+wgSIl9+EiJdfBI" +
  "iVXoSItV8EiLRfhIKcJIi0X4SIt16LkBAAAASInH6NEHAADJw1VIieVIg+wgSIl9+EiJdfBIiVXoSItV8EiLRfhIKcJIi3X4SItF" +
  "6LkBAAAASInH6JsHAADJw/MPHvpVSInlSIPsMEiJfdjohAUAAEiJRfhIi0X4SIPACL4BAAAASInH6EYJAABIiUXwSItF8EiDwEC+" +
  "AQAAAEiJx+gtCQAASIlF6EiLRfBIg8BIvgEAAABIicfoFAkAAEiJReBIi0XoSI1QBEiLRdhIg8AISI1IBEiLRdhIic5IicfoDf//" +
  "/4XAD4WwAAAASItF6EiNUBRIi0XYSIPAEEiNSARIi0XYSIPADEiJzkiJx+je/v//hcAPhYEAAABIi0XoSI1QMEiLRdhIg8AYSI1I" +
  "CEiLRdhIg8AYSInOSInH6K/+//+FwHVWSItF4EiNUBBIi0XYSIPAMEiNSAhIi0XYSIPAIEiJzkiJx+iE/v//hcB1K0iLRehIjVBY" +
  "SItF2EiDwEhIjUgISItF2EiDwDhIic5IicfoWf7//4XAdAe4/////+sFuAAAAADJw/MPHvpVSInlSIPsMEiJfdjoNwQAAEiJRfhI" +
  "i0X4SIPACL4BAAAASInH6PkHAABIiUXwSItF8EiDwEC+AQAAAEiJx+jgBwAASIlF6EiLRfBIg8BIvgEAAABIicfoxwcAAEiJReBI" +
  "i0XoSI1QBEiLRdhIg8AISI1IBEiLRdhIic5Iicfo9v3//4XAD4WwAAAASItF6EiNUBRIi0XYSIPAEEiNSARIi0XYSIPADEiJzkiJ" +
  "x+jH/f//hcAPhYEAAABIi0XoSI1QMEiLRdhIg8AYSI1ICEiLRdhIg8AYSInOSInH6Jj9//+FwHVWSItF4EiNUBBIi0XYSIPAMEiN" +
  "SAhIi0XYSIPAIEiJzkiJx+ht/f//hcB1K0iLRehIjVBYSItF2EiDwEhIjUgISItF2EiDwDhIic5IicfoQv3//4XAdAe4/////+sF" +
  "uAAAAADJw/MPHvpVSInlSIPsIEiJfejoofz//0iJRfhIg334AHUKuP/////pwwAAAOip/P//SIlF8EiDffAAdQq4/////+mpAAAA" +
  "SItF6EjHQEj/////SItF6Ei5EwAAAAAAAThIiUg4SItF6EjHQED/////SItF6MdAEAAAAABIi0Xoi1AQSItF6IlQDEiLReiLUAxI" +
  "i0XoiVAISItF6ItQCEiLReiJUARIi0Xoi1AESItF6IkQSItF6EiLVfhIiVAYSItF6EiLVfBIiVAwSItF6EiLUDBIi0XoSIlQKEiL" +
  "RehIi1AoSItF6EiJUCC4AAAAAMnD8w8e+lVIieVIg+wgSIl96EiJdeBIi0XgSItACEiJRfhIi0X4SIsASYnCSItF+EiDwDBMiwBI" +
  "i0X4SIPAKEiLOEiLRfhIg8AgSIsISItF+EiDwBhIixBIi0X4SIPAEEiLMEiLRfhIg8AISIsATYnBSYn4SInHQf/SSItV+EiJArgA" +
  "AAAAycNIx8ALAAAASYnKDwXD8w8e+lVIieVIgewgAQAASIm96P7//0iJtVj///9IiZVg////SImNaP///0yJhXD///9MiY14////" +
  "hMB0IA8pRYAPKU2QDylVoA8pXbAPKWXADylt0A8pdeAPKX3wx4Uw////CAAAAMeFNP///zAAAABIjUUQSImFOP///0iNhVD///9I" +
  "iYVA////SMeF8P7//wAAAABIx4X4/v//AAAAAEjHhQD///8AAAAASMeFCP///wAAAABIx4UQ////AAAAAEjHhRj///8AAAAASMeF" +
  "IP///wAAAABIi4Xo/v//SImF8P7//8eFTP///wEAAADrWouFMP///4P4L3cjSIuFQP///4uVMP///4nSSAHQi5Uw////g8IIiZUw" +
  "////6xJIi4U4////SI1QCEiJlTj///9IixCLhUz///9ImEiJlMXw/v//g4VM////AYO9TP///wZ+nUiNhfD+//9IicZIjT0I/v//" +
  "6In+//9Ii4Xw/v//ycNlSIsEJQAAAADD8w8e+lVIieVIjQXn////SInHuAAAAADoZ/7//13D8w8e+lVIieVIg+wQSI0NjicAALoA" +
  "AAAAvgEAAAC/AQAAAOgE6f//hcB0B7gAAAAA60joq////0iJwYsFZScAALoBAAAASInOicfofej//0iD+AF1IIsFRicAAEiNTf+6" +
  "AQAAAEiJzonH6Evo//+4AQAAAOsFuAAAAADJw1VIieWLBfsmAACFwHkL6HH///+JBewmAACLBeYmAACFwA+VwF3DVUiJ5UiJffiJ" +
  "dfSDffQAdRNIuP//////fwAASDlF+A+WwOttg330AXUxSLj//////3///0g5Rfh2F0i4//////7///9IOUX4dwe4AQAAAOsFuAAA" +
  "AACD4AHrNoN99AJ1K0i4//////7///9IOUX4dhFIgX34/+///3cHuAEAAADrBbgAAAAAg+AB6wW4AAAAAF3D8w8e+lVIieVIg+ww" +
  "SIl96EiJdeBIiVXY63u4QAAAAEiDfdhASA9GRdhIiUX4SItN4IsFOSYAAEiLVfhIic6Jx+hS5///SItV+Eg50HQHuP/////rUEiL" +
  "TeiLBQ0mAABIi1X4SInOicfoF+f//0iLVfhIOdB0B7j/////6yhIi0X4SAFF6EiLRfhIAUXgSItF+EgpRdhIg33YAA+Fev///7gA" +
  "AAAAycNIidHzpMPzDx76VUiJ5UiD7DBIiX3oSIl14EiJVdiJTdRIg33YAHUKuAAAAADprQEAAEiLRei+AAAAAEiJx+iD/v//hMB0" +
  "J0iLVehIi0XYSAHQSIPoAb4AAAAASInH6GP+//+EwHQHuAEAAADrBbgAAAAAiEX/gGX/AYtV1EiLReiJ1kiJx+g7/v//hMB0J0iL" +
  "VehIi0XYSAHQSI1Q/4tF1InGSInX6Bv+//+EwHQHuAEAAADrBbgAAAAAiEX+gGX+AUiLReC+AAAAAEiJx+jz/f//hMB0J0iLVeBI" +
  "i0XYSAHQSIPoAb4AAAAASInH6NP9//+EwHQHuAEAAADrBbgAAAAAiEX9gGX9AYtV1EiLReCJ1kiJx+ir/f//hMB0J0iLVeBIi0XY" +
  "SAHQSI1Q/4tF1InGSInX6Iv9//+EwHQHuAEAAADrBbgAAAAAiEX8gGX8AQ+2Rf+D8AGEwHQLD7ZF/oPwAYTAdRYPtkX9g/ABhMB0" +
  "Eg+2RfyD8AGEwHQHuP/////rWoB9/wB0DYB9/QB0B7j/////60foA/3//4TAdBlIi1XYSItN4EiLRehIic5Iicfoof3//+slSI09" +
  "P/7//0iLTdhIi1XgSItF6EiJxrgAAAAA6K/6//+4AAAAAMnD8w8e+lVIieVIg+wgSIl96Il15EiNRfiLVeRIi3XoidG6CAAAAEiJ" +
  "x+j4/f//hcB0CUjHwP/////rBEiLRfjJw/MPHvpVSInlSIPsIEiJffiJdfRIiVXoSI116ItV9EiLRfiJ0boIAAAASInH6LT9///J" +
  "wwAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAvc3lzdGVtL2NvbW1vbi9saWIvbGliU2NlU3lzVXRpbC5zcHJ4AAAAc2NlU3lzVXRpbFNlbmRTeXN0" +
  "ZW1Ob3RpZmljYXRpb25XaXRoVGV4dAAAAAAAAAAAL3N5c3RlbS9jb21tb24vbGliL2xpYlNjZVVzZXJTZXJ2aWNlLnNwcngAc2Nl" +
  "VXNlclNlcnZpY2VJbml0aWFsaXplAAAAAAAAc2NlVXNlclNlcnZpY2VHZXRGb3JlZ3JvdW5kVXNlcgBzY2VVc2VyU2VydmljZVRl" +
  "cm1pbmF0ZQAvc3lzdGVtL2NvbW1vbi9saWIvbGliU2NlQmdmdC5zcHJ4AHNjZUJnZnRTZXJ2aWNlSW50SW5pdAAAAAAAAAAAc2Nl" +
  "QmdmdFNlcnZpY2VEb3dubG9hZFJlZ2lzdGVyVGFzawAAAAAAAHNjZUJnZnRTZXJ2aWNlSW50RGVidWdEb3dubG9hZFJlZ2lzdGVy" +
  "UGtnAAAAAAAAAHNjZUJnZnRTZXJ2aWNlSW50RG93bmxvYWRTdGFydFRhc2sAc2NlQmdmdEZpbmFsaXplAAAAAAAAL3N5c3RlbS9j" +
  "b21tb24vbGliL2xpYlNjZUFwcEluc3RVdGlsLnNwcngAc2NlQXBwSW5zdFV0aWxJbml0aWFsaXplAERQSTogQXBwIEluc3QgVXRp" +
  "bCBFcnJvciAARFBJOiBCR0ZUIEluaXQgRmFpbGVkIAAAMABQUzRHRABEUEk6IEdFVCBJTkZPIEVSUk9SAERpcmVjdFBhY2thZ2VJ" +
  "bnN0YWxsZXIgRXhpdGVkAABEUEk6IEJHRlQgRXJyb3IgMHg4MDk5MDA4NgpFbnN1cmUgdGhhdCB0aGVyZSBhcmUgbm8gb2xkIGRv" +
  "d25sb2FkcyBpbiB0aGUgbm90aWZpY2F0aW9uIGxpc3QuAAAAAABEUEk6IFBhY2thZ2UgQWxyZWFkeSBJbnN0YWxsZWQhAERQSTog" +
  "SW5zdWZmaWNpZW50IHN0b3JhZ2Ugc3BhY2UuClBsZWFzZSBmcmVlIHVwIHNwYWNlIG9uIHlvdXIgaGFyZCBkcml2ZS4AAAAAAAAA" +
  "RFBJOiBJbnN1ZmZpY2llbnQgc3RvcmFnZSBzcGFjZS4KUGxlYXNlIGZyZWUgdXAgbm9uIGZyYWdtZW50ZWQgc3BhY2Ugb24geW91" +
  "ciBoYXJkIGRyaXZlLgBEUEk6IEJHRlQgRXJyb3IgAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAALS0tLS0tAAA/////w==";

function dpiPayload(ip, port) {
  const bin = atob(DPI_PAYLOAD_B64);
  const u = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) u[i] = bin.charCodeAt(i);
  let at = -1;
  for (let i = 0; i + 6 <= u.length; i++) {
    if (u[i] === 0xb4 && u[i+1] === 0xb4 && u[i+2] === 0xb4 && u[i+3] === 0xb4 && u[i+4] === 0xb4 && u[i+5] === 0xb4) { at = i; break; }
  }
  if (at < 0) return null;
  const parts = String(ip).split(".").map(Number);
  if (parts.length !== 4 || parts.some(n => !(n >= 0 && n <= 255))) return null;
  const pt = Number(port);
  if (!(pt >= 1 && pt <= 65535)) return null;
  u[at] = parts[0]; u[at+1] = parts[1]; u[at+2] = parts[2]; u[at+3] = parts[3];
  u[at+4] = (pt >> 8) & 255; u[at+5] = pt & 255;
  return u;
}

// Payload modificado (DPI, GPL v3): o pacote vem gravado dentro dele, sem servidor de retorno.
// Campos em JKJOB001: url[512] name[64] id[48] type[16] size(u64, little-endian).
const JK_PAYLOAD_B64 =
  "6TADAABmLg8fhAAAAAAAkFVIieVIgezQAQAASIm9SP7//4m1RP7//0iJlTj+//9IiY0w/v//x0X8AAAAAEiNVfxIi4VI/v//uQAA" +
  "AAC+AAAAAEiJx+hNAQAASIXAdAe4AAAAAOtfSMeFUP7//6gBAACLRfxIjZVQ/v//vgAAAACJx+gxAQAAhcB0B7gAAAAA6zOLRfSD" +
  "+AF3JkiLhXj///9JicBIi404/v//SIuFMP7//7oAAAAASInOSInHQf/Qi0X8SJjJw1VIieVIg+wQSIl9+Il19It19EiLRfi5AAAA" +
  "ALoAAAAASInH6CH////Jw1VIieVIg+wgSIl96EiJdeBIx0X4AAAAAEiLReiJwUiNVfhIi0XgSInGic/odQAAAEiLRfjJw2YPH4QA" +
  "AAAAALgDAAAASYnKDwUPgokAAADDuAQAAABJicoPBQ+CeAAAAMO4BQAAAEmJyg8FD4JnAAAAw7gGAAAASYnKDwUPglYAAADDuIcA" +
  "AABJicoPBQ+CRQAAAMO43QEAAEmJyg8FD4I0AAAAw7hPAgAASYnKDwUPgiMAAADDuFICAABJicoPBQ+CEgAAAMO4YAIAAEmJyg8F" +
  "D4IBAAAAw4kFiTQAAEgxwEj/yMO5AAgAAEgxwFDi/UiBxABAAADDVUiJ5Yl97EiJdeBIi0XgxgAwSItF4EiDwAHGAHjHRfwHAAAA" +
  "x0X4AgAAAOtTi0X8weACi1XsicHT+onQg+APiUX0g330CX8Zi0X0jUgwi0X4SGPQSItF4EgB0InKiBDrF4tF9I1IN4tF+Ehj0EiL" +
  "ReBIAdCJyogQg238AYNF+AGDffwAeadIi0XgSIPACsYAAJBdw1VIieVIiX3oSIl14EiJVdjHRfwAAAAAx0X4AAAAAOsqi0X4SGPQ" +
  "SItF6EiNDAKLRfyNUAGJVfxIY9BIi0XYSAHCD7YBiAKDRfgBi0X4SGPQSItF6EgB0A+2AITAdcLHRfQAAAAA6yqLRfRIY9BIi0Xg" +
  "SI0MAotF/I1QAYlV/Ehj0EiLRdhIAcIPtgGIAoNF9AGLRfRIY9BIi0XgSAHQD7YAhMB1wotF/Ehj0EiLRdhIAdDGAACQXcNVSInl" +
  "SIHskAMAAEiNhTD///9IicfozgsAAEiNhTD///9IicfoUQ4AAEjHhWD///8AAAAASLgQAAAAAAAAOEiJhWj///9IuAAAAAAAHABA" +
  "SImFcP///0i4AP8AAAAAAJBIiYV4////SI2FMP///0iJx+i7DAAA6Cb+//++AAAAAEiNBUQcAABIicfoA/3//0iJRfhIi0X4SI0V" +
  "VRwAAEiJ1kiJx+gT/f//SIlF8L4AAAAASI0FahwAAEiJx+jR/P//SIlF6EiLRehIjRV9HAAASInWSInH6OH8//9IiUXgSItF6EiN" +
  "FYEcAABIidZIicfox/z//0iJRdhIi0XoSI0VhxwAAEiJ1kiJx+it/P//SIlF0L4AAAAASI0FhBwAAEiJx+hr/P//SIlFyEiLRchI" +
  "jRWQHAAASInWSInH6Hv8//9IiUXASItFyEiNFZMcAABIidZIicfoYfz//0iJRbhIi0XISI0VoRwAAEiJ1kiJx+hH/P//SIlFsEiL" +
  "RchIjRW3HAAASInWSInH6C38//9IiUWoSItFyEiNFcAcAABIidZIicfoE/z//0iJRaDo7/z//8eFLP///7wCAADHhSj///8AAAAA" +
  "SI2FLP///0iLVeBIicf/0kiNhSj///9Ii1XYSInH/9JIi0XQ/9C+AAAAAEiNBXscAABIicfokvv//0iJRZhIi0WYSI0VjhwAAEiJ" +
  "1kiJx+ii+///SIlFkEiLRZD/0IlFjIN9jAAPhJYAAABIjZWQ/P//uAAAAAC5QAAAAEiJ1/NIq0jHhXD8//8AAAAASMeFePz//wAA" +
  "AABIx4WA/P//AAAAAEjHhYj8//8AAAAASI2VcPz//4tFjEiJ1onH6Cz8//9IjZWQ/P//SI2FcPz//0iJxkiNBRYcAABIicfooPz/" +
  "/0iNhZD8//9Ii1XwSInGv94AAAD/0rj/////6bEDAABBuQAAAABBuP////+5AhAAALoDAAAAvgAAEAC/AAAAAOhk+///SImFEP//" +
  "/0jHhRj///8AABAASI2FEP///0iLVcBIicf/0olFjIN9jAAPhKMAAACBfYwBAJmAD4SWAAAASI2VkPz//7gAAAAAuUAAAABIidfz" +
  "SKtIx4Vw/P//AAAAAEjHhXj8//8AAAAASMeFgPz//wAAAABIx4WI/P//AAAAAEiNlXD8//+LRYxIidaJx+g1+///SI2VkPz//0iN" +
  "hXD8//9IicZIjQU5GwAASInH6Kn7//9IjYWQ/P//SItV8EiJxr/eAAAA/9K4/////+m6AgAASI2VoP7//7gAAAAAuQ0AAABIidfz" +
  "SKuLhSj///+JhaD+///HhaT+//8FAAAASI0F9RoAAEiJhaj+//9IjQXnGgAASImFsP7//0iNBdkaAABIiYXA/v//SI0FyxoAAEiJ" +
  "hcj+///Hhdj+//8AAAEASI0FtBoAAEiJheD+//9IjQWoGgAASImF8P7//0iNBZcaAABIiYX4/v//SI2FoP7//0iJx+j6BAAAiUWM" +
  "g32M/3UfSItF8EiNFXYaAABIida/3gAAAP/QuP/////p6gEAAIN9jAF1H0iLRfBIjRVlGgAASInWv94AAAD/0LgAAAAA6cUBAADH" +
  "hZz+////////SI2VnP7//0iNhaD+//9Ii024SInWSInH/9GJRYyBfYyIAJmAdCGLhZz+//+D+P90FouFnP7//0iLVaiJx//SiUWM" +
  "6W8BAABIjZWc/v//SI2FoP7//0iLTbBIidZIicf/0YlFjIF9jIgAmYB0IYuFnP7//4P4/3QWi4Wc/v//SItVqInH/9KJRYzpKAEA" +
  "AIF9jIYAmYB1GkiLRfBIjRXEGQAASInWv94AAAD/0OkFAQAAgX2MiACZgHUaSItF8EiNFQEaAABIida/3gAAAP/Q6eIAAACBfYw5" +
  "AJmAdAmBfYwmAKOAdRpIi0XwSI0V9RkAAEiJ1r/eAAAA/9DptgAAAIF9jIUAmYB1GkiLRfBIjRUiGgAASInWv94AAAD/0OmTAAAA" +
  "SI2VkPz//7gAAAAAuUAAAABIidfzSKtIx4Vw/P//AAAAAEjHhXj8//8AAAAASMeFgPz//wAAAABIx4WI/P//AAAAAEiNlXD8//+L" +
  "RYxIidaJx+h9+P//SI2VkPz//0iNhXD8//9IicZIjQX9GQAASInH6PH4//9IjYWQ/P//SItV8EiJxr/eAAAA/9K4/////+sF6d/9" +
  "///Jw1VIieVIg+wgiX3sSI1N/ItF7LoEAAAASInOicfoYPf//0iNRfyLAMnDVUiJ5UiD7CCJfexIjU34i0XsuggAAABIic6Jx+g3" +
  "9///SI1F+EiLAMnDVUiJ5UiJfejHRfwAAAAA6wSDRfwBi0X8SGPQSItF6EgB0A+2AITAdeiLRfxdw1VIieVIg+wgiX3sSIl14IlV" +
  "6ItF7InH6GP///+JRfxIi0XgxgAAi0XoO0X8cwe4/////+sZi1X8SItN4ItF7EiJzonH6Lf2//+4AAAAAMnDVUiJ5UiD7DBIiX3Y" +
  "SIl10EiLRdhIicfoZv///4lF+ItF+Ehj0EiLRdhIAdBIiUXwSItF0EiJx+hG////iUXsx0X8AAAAAOsji0X8SGPQSItF0EgB0ItV" +
  "/EhjykiLVfBIAcoPtgCIAoNF/AGLRfw7Rex81ZCQycNVSInlSIPsIEiJfehIiXXgSItF6MYAAEi4L3VzZXIvZGFIunRhL3RtcF8A" +
  "SIlF8EiJVfhIjVXwSItF6EiJ1kiJx+hD////SItV4EiLRehIidZIicfoMP///5DJw1VIieVIgewAEAAASIMMJABIg+xQib3M7///" +
  "SIm1wO///0iJlbjv//9Ii4XA7///xgAAi4XM7///icfoHf7//4lF/IN9/AB1CrgAAAAA6e4AAABIi5W47///SIuFwO///0iJ1kiJ" +
  "x+g5////SIuFwO///7r/AQAAvgEGAABIice4AAAAAOh29f//iUXkx0XgABAAAEjHRfD/////612LRfxIi1XwKdCJRfxIjYXQ7///" +
  "SIlF6Os3SItV8EiLTeiLReRIic6Jx+gj9f//SIlF2EiDfdgAfwe4AQAAAOtjSItF2EgBRehIi0XYSClF8EiDffAAf8KDffwAfjaL" +
  "VeCLRfw5wg9Owkhj0EiNjdDv//+Lhczv//9Iic6Jx+i+9P//SIlF8EiDffAAD49v////6wGQi0Xkicfo1fT//7gBAAAAycNVSInl" +
  "SIl9+IsFuSkAAIXAdAe4AQAAAOt7xwWkKQAAAQAAAA+2BQknAACEwHUHuP/////rX0iLRfhIjRUzKQAASIlQCEiLRfhIjRXkJgAA" +
  "SIlQEEiLRfhIjRXVKAAASIlQIEiLFUopAABIi0X4SIlQYEiLRfhIjRVPKQAASIlQKEiLRfhIjRUYKQAASIlQULgAAAAAXcNVSInl" +
  "SIPsQOhBBwAASIlF8EiLRfBIg8AIvgEAAABIicfoAAsAAEiJRfhIi0X4SI2wsAAAAEiNRcy5AQAAALoEAAAASInH6AQJAACFwA+F" +
  "pgAAAItFzIP4AXVZSItF+EiDwEC+AQAAAEiJx+i2CgAASIlF2EiLRfhIg8BIvgEAAABIicfonQoAAEiJRdBIi0XYSI1wMEiNBaEo" +
  "AAC5AQAAALoIAAAASInH6KEIAACFwHVT61hIi0X4vgEAAABIicfoYQoAAEiJRehIi0XoSIPACL4BAAAASInH6EgKAABIiUXgSItF" +
  "4Eg7Rfh1E0iLRehIiUX46TH///+Q6Qn///+Q6QP///+4/////+s8SItF0EiNcBhIjQUqKAAAuQEAAAC6CAAAAEiJx+giCAAAhcB0" +
  "EkjHBQEoAAAAAAAAuP/////rBbgAAAAAycNVSInlSIsF6CcAAEiFwHUF6KH+//9IiwXXJwAAXcNVSInlSIsF0icAAEiFwHUF6IP+" +
  "//9IiwXBJwAAXcNVSInlSIPsIEiJffhIiXXwSIlV6EiLVfBIi0X4SCnCSItF+EiLdei5AQAAAEiJx+iVBwAAycNVSInlSIPsIEiJ" +
  "ffhIiXXwSIlV6EiLVfBIi0X4SCnCSIt1+EiLRei5AQAAAEiJx+hfBwAAycNVSInlSIPsMEiJfdjoSwUAAEiJRfhIi0X4SIPACL4B" +
  "AAAASInH6AoJAABIiUXwSItF8EiDwEC+AQAAAEiJx+jxCAAASIlF6EiLRfBIg8BIvgEAAABIicfo2AgAAEiJReBIi0XoSI1QBEiL" +
  "RdhIg8AISI1IBEiLRdhIic5IicfoEf///4XAD4WwAAAASItF6EiNUBRIi0XYSIPAEEiNSARIi0XYSIPADEiJzkiJx+ji/v//hcAP" +
  "hYEAAABIi0XoSI1QMEiLRdhIg8AYSI1ICEiLRdhIg8AYSInOSInH6LP+//+FwHVWSItF4EiNUBBIi0XYSIPAMEiNSAhIi0XYSIPA" +
  "IEiJzkiJx+iI/v//hcB1K0iLRehIjVBYSItF2EiDwEhIjUgISItF2EiDwDhIic5IicfoXf7//4XAdAe4/////+sFuAAAAADJw1VI" +
  "ieVIg+wwSIl92OgCBAAASIlF+EiLRfhIg8AIvgEAAABIicfowQcAAEiJRfBIi0XwSIPAQL4BAAAASInH6KgHAABIiUXoSItF8EiD" +
  "wEi+AQAAAEiJx+iPBwAASIlF4EiLRehIjVAESItF2EiDwAhIjUgESItF2EiJzkiJx+j+/f//hcAPhbAAAABIi0XoSI1QFEiLRdhI" +
  "g8AQSI1IBEiLRdhIg8AMSInOSInH6M/9//+FwA+FgQAAAEiLRehIjVAwSItF2EiDwBhIjUgISItF2EiDwBhIic5IicfooP3//4XA" +
  "dVZIi0XgSI1QEEiLRdhIg8AwSI1ICEiLRdhIg8AgSInOSInH6HX9//+FwHUrSItF6EiNUFhIi0XYSIPASEiNSAhIi0XYSIPAOEiJ" +
  "zkiJx+hK/f//hcB0B7j/////6wW4AAAAAMnDVUiJ5UiD7CBIiX3o6LX8//9IiUX4SIN9+AB1Crj/////6cMAAADoufz//0iJRfBI" +
  "g33wAHUKuP/////pqQAAAEiLRehIx0BI/////0iLRehIuRMAAAAAAAE4SIlIOEiLRehIx0BA/////0iLRejHQBAAAAAASItF6ItQ" +
  "EEiLReiJUAxIi0Xoi1AMSItF6IlQCEiLReiLUAhIi0XoiVAESItF6ItQBEiLReiJEEiLRehIi1X4SIlQGEiLRehIi1XwSIlQMEiL" +
  "RehIi1AwSItF6EiJUChIi0XoSItQKEiLRehIiVAguAAAAADJw1VIieVIg+wgSIl96EiJdeBIi0XgSItACEiJRfhIi0X4SIsASYnC" +
  "SItF+EiDwDBMiwBIi0X4SIPAKEiLOEiLRfhIg8AgSIsISItF+EiDwBhIixBIi0X4SIPAEEiLMEiLRfhIg8AISIsATYnBSYn4SInH" +
  "Qf/SSItV+EiJArgAAAAAycNIx8ALAAAASYnKDwXDVUiJ5UiB7CABAABIib3o/v//SIm1WP///0iJlWD///9IiY1o////TImFcP//" +
  "/0yJjXj///+EwHQgDylFgA8pTZAPKVWgDyldsA8pZcAPKW3QDyl14A8pffDHhTD///8IAAAAx4U0////MAAAAEiNRRBIiYU4////" +
  "SI2FUP///0iJhUD///9mD+/ADymF8P7//w8phQD///8PKYUQ////Zg/WhSD///9Ii4Xo/v//SImF8P7//8eFTP///wEAAADrWouF" +
  "MP///4P4L3cjSIuFQP///4uVMP///4nSSAHQi5Uw////g8IIiZUw////6xJIi4U4////SI1QCEiJlTj///9IixCLhUz///9ImEiJ" +
  "lMXw/v//g4VM////AYO9TP///wZ+nUiNhfD+//9IicZIjQU8/v//SInH6Lb+//9Ii4Xw/v//ycNlSIsEJQAAAADDVUiJ5UiNBev/" +
  "//9Iice4AAAAAOiY/v//XcNVSInlSIPsEEiNBdYhAABIicG6AAAAAL4BAAAAvwEAAADou+z//4XAdAe4AAAAAOtI6LD///9IicGL" +
  "BaohAAC6AQAAAEiJzonH6GDs//9Ig/gBdSCLBYshAABIjU3/ugEAAABIic6Jx+gw7P//uAEAAADrBbgAAAAAycNVSInliwU4IQAA" +
  "hcB5C+hy////iQUpIQAAiwUjIQAAhcAPlcBdw1VIieVIiX34iXX0g330AHUTSLj//////38AAEg7RfgPk8DrbYN99AF1MUi4////" +
  "//9///9IO0X4cxdIuP/////+////SDtF+HIHuAEAAADrBbgAAAAAg+AB6zaDffQCdStIuP/////+////SDtF+HMRSIF9+P/v//93" +
  "B7gBAAAA6wW4AAAAAIPgAesFuAAAAABdw1VIieVIg+wwSIl96EiJdeBIiVXY6YIAAABIi0XYukAAAABIOdBID0fCSIlF+EiLTeCL" +
  "BX4gAABIi1X4SInOicfoNev//0iJwkiLRfhIOcJ0B7j/////61NIi03oiwVPIAAASItV+EiJzonH6Pnq//9IicJIi0X4SDnCdAe4" +
  "/////+soSItF+EgBRehIi0X4SAFF4EiLRfhIKUXYSIN92AAPhXP///+4AAAAAMnDSInR86TDVUiJ5UiD7DBIiX3oSIl14EiJVdiJ" +
  "TdRIg33YAHUKuAAAAADprQEAAEiLRei+AAAAAEiJx+iB/v//hMB0J0iLVehIi0XYSAHQSIPoAb4AAAAASInH6GH+//+EwHQHuAEA" +
  "AADrBbgAAAAAiEX/gGX/AYtV1EiLReiJ1kiJx+g5/v//hMB0J0iLVehIi0XYSAHQSI1Q/4tF1InGSInX6Bn+//+EwHQHuAEAAADr" +
  "BbgAAAAAiEX+gGX+AUiLReC+AAAAAEiJx+jx/f//hMB0J0iLVeBIi0XYSAHQSIPoAb4AAAAASInH6NH9//+EwHQHuAEAAADrBbgA" +
  "AAAAiEX9gGX9AYtV1EiLReCJ1kiJx+ip/f//hMB0J0iLVeBIi0XYSAHQSI1Q/4tF1InGSInX6In9//+EwHQHuAEAAADrBbgAAAAA" +
  "iEX8gGX8AQ+2Rf+D8AGEwHQLD7ZF/oPwAYTAdRYPtkX9g/ABhMB0Eg+2RfyD8AGEwHQHuP/////rWoB9/wB0DYB9/QB0B7j/////" +
  "60foAf3//4TAdBlIi1XYSItN4EiLRehIic5Iicfon/3//+slSI09Q/7//0iLTdhIi1XgSItF6EiJxrgAAAAA6N/6//+4AAAAAMnD" +
  "VUiJ5UiD7CBIiX3oiXXkSI1F+ItV5EiLdeiJ0boIAAAASInH6AD+//+FwHQJSMfA/////+sESItF+MnDVUiJ5UiD7CBIiX34iXX0" +
  "SIlV6EiNdeiLVfRIi0X4idG6CAAAAEiJx+jA/f//ycMAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAvc3lzdGVtL2NvbW1vbi9saWIvbGliU2NlU3lzVXRpbC5zcHJ4AAAAc2NlU3lzVXRpbFNlbmRTeXN0" +
  "ZW1Ob3RpZmljYXRpb25XaXRoVGV4dAAAAAAAAAAAL3N5c3RlbS9jb21tb24vbGliL2xpYlNjZVVzZXJTZXJ2aWNlLnNwcngAc2Nl" +
  "VXNlclNlcnZpY2VJbml0aWFsaXplAAAAAAAAc2NlVXNlclNlcnZpY2VHZXRGb3JlZ3JvdW5kVXNlcgBzY2VVc2VyU2VydmljZVRl" +
  "cm1pbmF0ZQAvc3lzdGVtL2NvbW1vbi9saWIvbGliU2NlQmdmdC5zcHJ4AHNjZUJnZnRTZXJ2aWNlSW50SW5pdAAAAAAAAAAAc2Nl" +
  "QmdmdFNlcnZpY2VEb3dubG9hZFJlZ2lzdGVyVGFzawAAAAAAAHNjZUJnZnRTZXJ2aWNlSW50RGVidWdEb3dubG9hZFJlZ2lzdGVy" +
  "UGtnAAAAAAAAAHNjZUJnZnRTZXJ2aWNlSW50RG93bmxvYWRTdGFydFRhc2sAc2NlQmdmdEZpbmFsaXplAAAAAAAAL3N5c3RlbS9j" +
  "b21tb24vbGliL2xpYlNjZUFwcEluc3RVdGlsLnNwcngAc2NlQXBwSW5zdFV0aWxJbml0aWFsaXplAERQSTogQXBwIEluc3QgVXRp" +
  "bCBFcnJvciAARFBJOiBCR0ZUIEluaXQgRmFpbGVkIAAAMABQUzRHRABEUEk6IEdFVCBJTkZPIEVSUk9SAERpcmVjdFBhY2thZ2VJ" +
  "bnN0YWxsZXIgRXhpdGVkAABEUEk6IEJHRlQgRXJyb3IgMHg4MDk5MDA4NgpFbnN1cmUgdGhhdCB0aGVyZSBhcmUgbm8gb2xkIGRv" +
  "d25sb2FkcyBpbiB0aGUgbm90aWZpY2F0aW9uIGxpc3QuAAAAAABEUEk6IFBhY2thZ2UgQWxyZWFkeSBJbnN0YWxsZWQhAERQSTog" +
  "SW5zdWZmaWNpZW50IHN0b3JhZ2Ugc3BhY2UuClBsZWFzZSBmcmVlIHVwIHNwYWNlIG9uIHlvdXIgaGFyZCBkcml2ZS4AAAAAAAAA" +
  "RFBJOiBJbnN1ZmZpY2llbnQgc3RvcmFnZSBzcGFjZS4KUGxlYXNlIGZyZWUgdXAgbm9uIGZyYWdtZW50ZWQgc3BhY2Ugb24geW91" +
  "ciBoYXJkIGRyaXZlLgBEUEk6IEJHRlQgRXJyb3IgAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAALS0tLS0tAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAASktKT0IwMDEAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" +
  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAD/////";

function jkPayload(urlStr, name, id, type, size) {
  const bin = atob(JK_PAYLOAD_B64);
  const u = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) u[i] = bin.charCodeAt(i);
  const magic = "JKJOB001";
  let at = -1;
  for (let i = 0; i + 8 <= u.length && at < 0; i++) {
    let ok = true;
    for (let k = 0; k < 8; k++) if (u[i + k] !== magic.charCodeAt(k)) { ok = false; break; }
    if (ok) at = i;
  }
  if (at < 0) return null;
  const put = (off, max, str) => {
    const b = new TextEncoder().encode(str);
    if (b.length >= max) throw new Error("campo grande demais");
    for (let k = 0; k < max; k++) u[at + off + k] = k < b.length ? b[k] : 0;
  };
  put(8, 512, urlStr); put(520, 64, name); put(584, 48, id); put(632, 16, type);
  let v = BigInt(size);
  for (let k = 0; k < 8; k++) { u[at + 648 + k] = Number(v & 255n); v >>= 8n; }
  return u;
}

const INSTALAR_HTML = `<!DOCTYPE html><html><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>JK PATCHS - Instalar</title>
<style>body{margin:0;padding:30px;background:#0b0f1a;color:#e8ecf4;font-family:Arial,sans-serif;font-size:26px}
.b{display:block;margin:18px 0;padding:26px 14px;background:#1fbf6b;color:#06210f;font-weight:bold;text-decoration:none;text-align:center;border-radius:12px;font-size:26px}
pre{background:#141b2d;padding:16px;border-radius:10px;font-size:22px;white-space:pre-wrap}</style></head><body>
<h2>Instalar o JK PATCHS</h2>
<p>Deixe <b>Enable Payload Server</b> ativo no GoldHEN e toque no bot&atilde;o. O download come&ccedil;a sozinho; acompanhe em Notifica&ccedil;&otilde;es.</p>
<p style="font-size:20px;color:#9fb0d0">Se o JK PATCHS j&aacute; estiver instalado, exclua-o antes (o PS4 n&atilde;o instala por cima).</p>
<div id="avancado" style="display:none">
<a class="b" href="javascript:enviar(1)" onclick="enviar(1);return false;">TESTE 1 - so enviar o payload (sem servidor)</a>
<a class="b" href="javascript:enviar(2)" onclick="enviar(2);return false;">TESTE 2 - instalar o JK PATCHS (precisa do servidor)</a>
</div>
<a class="b" href="javascript:enviar(3)" onclick="enviar(3);return false;">INSTALAR JK PATCHS</a>
<pre id="saida">Pronto. Toque em INSTALAR JK PATCHS.</pre>
<script>
var JKOK=1;try{if(location.search.indexOf("teste=1")>=0)document.getElementById("avancado").style.display="block";}catch(e){}window.onerror=function(m){try{document.getElementById("saida").appendChild(document.createTextNode("\\nERRO JS: "+m));}catch(e){}};
function mostra(t){document.getElementById("saida").appendChild(document.createTextNode("\\n"+t));}
function qs(k){var m=location.search.match(new RegExp("[?&]"+k+"=([^&]*)"));return m?decodeURIComponent(m[1]):"";}
function enviar(modo){
  var ip="127.0.0.1",port="1",alvo="/p.bin?ip=";
  if(modo===2){ip=qs("ip");port=qs("port");if(!ip||!port){mostra("Falta ?ip=...&port=... na URL.");return;}}
  mostra("Baixando payload...");
  var x=new XMLHttpRequest();
  if(modo===3){x.open("GET","/q.bin",true);}else{x.open("GET","/p.bin?ip="+encodeURIComponent(ip)+"&port="+encodeURIComponent(port),true);}
  x.responseType="arraybuffer";
  x.onload=function(){
    if(x.status!==200){mostra("Erro "+x.status+" ao baixar o payload.");return;}
    mostra("Enviando para o PS4 (127.0.0.1:9090)...");
    var y=new XMLHttpRequest();
    var done=false;
    function fim(m){if(!done){done=true;mostra(m);}}
    y.open("POST","http://127.0.0.1:9090/",true);
    y.setRequestHeader("Content-Type","text/plain");
    y.onload=function(){fim("Enviado (resposta "+y.status+").");};
    y.onerror=function(){fim("Enviado (sem resposta, normal). Olhe se apareceu notificacao 'DPI:' no PS4.");};
    y.timeout=8000;
    y.ontimeout=function(){fim("Enviado (tempo esgotado, normal). Olhe se apareceu notificacao 'DPI:' no PS4.");};
    try{y.send(x.response);}catch(e){fim("Falha ao enviar: "+e);}
  };
  x.onerror=function(){mostra("Falha ao baixar o payload.");};
  x.send();
}
</script></body></html>`;

// ---------- DOWNLOAD PUBLICO RESTRITO ----------
// Somente este arquivo do R2 pode ser baixado sem login.
// O bucket inteiro continua privado.
async function downloadApp(request, env, variant) {
  if (
    request.method !== "GET" &&
    request.method !== "HEAD"
  ) {
    return new Response("Metodo nao permitido", {
      status: 405,
      headers: {
        Allow: "GET, HEAD"
      }
    });
  }

  if (!env.APP_BUCKET) {
    return text(500, "ERRO\nAPP_BUCKET nao configurado");
  }

  // t4 = arquivo de teste (APP-PS4/TESTE.pkg), so para diagnostico
  const key = variant === "t4" ? "APP-PS4/TESTE.pkg" : "APP-PS4/JKPATCHS.pkg";

  // Suporta "Range" (download em partes / retomar), como os navegadores e gerenciadores de download esperam.
  const rangeHdr = request.headers.get("Range");

  let object;

  if (request.method === "HEAD") {
    object = await env.APP_BUCKET.head(key);
  } else if (rangeHdr) {
    object = await env.APP_BUCKET.get(key, { range: request.headers });
  } else {
    object = await env.APP_BUCKET.get(key);
  }

  if (!object) {
    return text(404, "ERRO\nInstalador nao encontrado");
  }

  const headers = new Headers();

  object.writeHttpMetadata(headers);

  // variantes de teste para o navegador do PS4: t1 = sem Content-Disposition,
  // t2 = com attachment, t3 = tipo x-ps4-pkg sem Content-Disposition
  if (variant === "t3") {
    headers.set("Content-Type", "application/x-ps4-pkg");
  } else {
    headers.set("Content-Type", "application/octet-stream");
  }
  if (variant !== "t1" && variant !== "t3") {
    headers.set("Content-Disposition", 'attachment; filename="JKPATCHS.pkg"');
  }
  headers.set("Accept-Ranges", "bytes");
  headers.set("Cache-Control", "no-store");
  headers.set("X-Content-Type-Options", "nosniff");

  if (object.httpEtag) {
    headers.set("ETag", object.httpEtag);
  }

  let status = 200;
  let length = object.size;

  if (rangeHdr && object.range && request.method !== "HEAD") {
    const r = object.range;
    const offset = r.offset !== undefined ? r.offset : 0;
    length = r.length !== undefined ? r.length : object.size - offset;

    headers.set(
      "Content-Range",
      "bytes " + offset + "-" + (offset + length - 1) + "/" + object.size
    );

    status = 206;
  }

  headers.set("Content-Length", String(length));

  return new Response(
    request.method === "HEAD" ? null : object.body,
    { status, headers }
  );
}

// ---------- PAGINA DE DOWNLOAD (publica) ----------
// Acesse em /baixar. Fica no proprio worker: gratis e sem depender do r2.dev.
const LANDING_HTML = `<!DOCTYPE html>
<html lang="pt-BR">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>JK PATCHS - Baixar o aplicativo</title>
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=Barlow+Condensed:wght@600;700;800&display=swap">
<style>
*{box-sizing:border-box}
body{margin:0;background:#000;color:#f1f3f8;font-family:Arial,Helvetica,sans-serif;font-size:18px;line-height:1.5}
a{color:inherit}
.w{max-width:1760px;margin:0 auto;padding:0 4%}
.dsp{font-family:"Barlow Condensed","Arial Narrow",Impact,Arial,sans-serif;text-transform:uppercase}
.eb{font:600 13px "Courier New",monospace;letter-spacing:2px;text-transform:uppercase;color:#4d9bff}
h1,h2,h3{margin:0;font-family:"Barlow Condensed","Arial Narrow",Impact,Arial,sans-serif;text-transform:uppercase;line-height:1.02}
h1{font-size:104px;font-weight:800}
h2{font-size:56px;font-weight:700}
h3{font-size:30px;font-weight:700}
.hero{padding-top:44px;padding-bottom:30px}
.top{display:table;width:100%;margin-bottom:34px}
.top>div{display:table-cell;vertical-align:middle}
.lg{width:120px;height:120px;border-radius:50%;display:block}
.lead{max-width:900px;font-size:22px;color:#9aa1b3;margin:22px 0 30px}
.btn{display:inline-block;font-weight:bold;font-size:18px;text-decoration:none;text-align:center;padding:15px 26px;border-radius:10px;margin:0 8px 10px 0;border:1px solid #23262f;color:#f1f3f8;background:#0d0e12}
.btn.g{background:#1fbf6b;color:#04210f;border-color:#1fbf6b;font-size:22px;padding:20px 38px}
.btn.g:hover{background:#2ad67b}
.st{font-size:14px;color:#8a92a6;margin:4px 0 0}
.score{display:table;width:100%;table-layout:fixed;margin-top:38px;border:1px solid #23262f;border-radius:12px;background:#0d0e12}
.score>div{display:table-cell;padding:24px 28px;border-right:1px solid #23262f}
.score>div:last-child{border-right:0}
.score b{display:block;font-family:"Barlow Condensed","Arial Narrow",Arial,sans-serif;font-weight:800;font-size:76px;line-height:1}
.score span{font:600 12px "Courier New",monospace;letter-spacing:1.5px;text-transform:uppercase;color:#9aa1b3}
section{padding:64px 0}
.cards{font-size:0;margin:6px -8px 0}
.cd{display:inline-block;vertical-align:top;width:33.333%;padding:8px;font-size:16px}
.cd>div{background:#0d0e12;border:1px solid #23262f;border-radius:14px;padding:16px 20px;min-height:0}
.cd.f>div{background:#0b1220;border-color:#2a4f8f}
.cd i{font:600 12px "Courier New",monospace;font-style:normal;letter-spacing:1.5px;text-transform:uppercase;color:#9aa1b3}
.cd h3{margin:8px 0 10px}
.cd p{margin:0;color:#9aa1b3;font-size:17px}
.cd em{display:block;font:400 12px "Courier New",monospace;font-style:normal;color:#7d8599;margin-top:12px}
video{display:block;width:100%;max-width:1200px;margin-top:26px;border:1px solid #23262f;border-radius:14px;background:#111;min-height:220px}
.band{background:#0d0e12;border-top:1px solid #23262f;border-bottom:1px solid #23262f}
.two{display:table;width:100%;table-layout:fixed}
.two>div{display:table-cell;vertical-align:middle}
.two>div.l{padding-right:36px}
.steps{margin:24px 0 0;padding:0;list-style:none}
.steps li{margin:0 0 18px;padding-left:58px;position:relative;min-height:44px}
.steps li b{display:block}
.steps li span{color:#9aa1b3;font-size:17px}
.steps .n{position:absolute;left:0;top:0;width:44px;height:44px;line-height:42px;text-align:center;border:1px solid #23262f;border-radius:10px;background:#000;font:800 22px "Barlow Condensed","Arial Narrow",Arial,sans-serif}
.legend{margin-top:6px;font-size:14px;color:#9aa1b3}
.legend span{display:inline-block;margin-right:16px}
.legend i{display:inline-block;width:10px;height:10px;border-radius:3px;margin-right:7px}
.win{border:1px solid #23262f;border-radius:14px;background:#000;overflow:hidden}
.win .bar{padding:10px 14px;border-bottom:1px solid #23262f;font:12px "Courier New",monospace;color:#9aa1b3}
.win .cols{display:table;width:100%;table-layout:fixed;min-height:250px}
.win .col{display:table-cell;vertical-align:top;padding:14px;border-right:1px solid #23262f}
.win .col:last-child{border-right:0}
.win h4{margin:0 0 8px;font:700 12px "Courier New",monospace;letter-spacing:1.5px;text-transform:uppercase}
.win .info{font-size:14px;color:#9aa1b3}
.win .info b{display:block;color:#f1f3f8;font-family:"Barlow Condensed","Arial Narrow",Arial,sans-serif;text-transform:uppercase;font-size:20px;margin-bottom:5px}
.chip{display:block;font-size:14.5px;padding:8px 10px;border-radius:8px;border:1px solid;margin-bottom:8px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.chip.g{color:#35d07f;border-color:#1d6e45;background:#07150e}
.chip.y{color:#f5c542;border-color:#7a6320;background:#161204}
.win .gh{color:#35d07f}.win .yh{color:#f5c542}
.acc{display:table;width:100%;table-layout:fixed;background:#0d0e12;border:1px solid #23262f;border-radius:16px}
.acc>div{display:table-cell;vertical-align:middle;padding:44px}
.qrw{display:table;margin-top:22px}
.qrw>div{display:table-cell;vertical-align:middle}
.qr{display:block;width:220px;height:220px;background:#fff;padding:10px;border-radius:12px}
.qrw .t{padding-left:20px}
.qrw .t b{display:block;font-family:"Barlow Condensed","Arial Narrow",Arial,sans-serif;text-transform:uppercase;font-size:22px;margin-bottom:4px}
.qrw .t span{color:#9aa1b3;font-size:15px}
.chk{margin:0;padding:0;list-style:none;color:#9aa1b3;font-size:19px}
.chk li{margin-bottom:12px}
.chk li:before{content:"\\2713  ";color:#35d07f;font-weight:bold}
.plans{margin-top:10px;color:#9aa1b3}
.plans b{color:#f1f3f8}
.foot{text-align:center;color:#62708f;font-size:13px;padding:30px 18px 44px}
@media (max-width:860px){
 h1{font-size:46px}h2{font-size:32px}h3{font-size:24px}.lead{font-size:18px}.btn.g{font-size:19px;padding:17px 24px}.score b{font-size:36px}
 .cd{width:100%}
 .two,.two>div,.acc,.acc>div,.win .cols,.win .col{display:block}
 .two>div.l{padding-right:0;margin-bottom:26px}
 .win .col{border-right:0;border-bottom:1px solid #23262f}
 .score b{font-size:36px}.score>div{padding:12px}
 .acc>div{padding:22px}
}

.nav{border-bottom:1px solid #23262f;background:#000}
.nav .w{display:table;width:100%;padding-top:14px;padding-bottom:14px}
.nav .w>div{display:table-cell;vertical-align:middle}
.nav img.lg{width:64px;height:64px}
.tabs{text-align:right}
.tab{display:inline-block;font-weight:bold;font-size:17px;text-decoration:none;padding:13px 24px;border-radius:10px;margin-left:8px;border:1px solid #23262f;color:#9aa1b3;background:#0d0e12}
.tab.on{color:#fff;border-color:#4d9bff;background:#0b1830}
.p1{display:table;width:100%;table-layout:fixed;padding-top:34px;padding-bottom:30px}
.p1>div{display:table-cell;vertical-align:top}
.p1>div.l{width:50%;padding-right:44px}
.p1 h1{font-size:46px}.p1b{width:100%;table-layout:fixed;border-collapse:separate;border-spacing:0}.p1b td{vertical-align:top;height:1px}.p1b td.bl{width:50%;padding-right:22px}.p1b td.br{padding-left:22px}.p1b .score,.p1b .side{margin:0;height:100%;box-sizing:border-box}.p1b .score{display:table;height:100%}.p1b .score>div{vertical-align:middle;padding:10px 20px}.p1b .score b{font-size:46px}.p1b .side{padding:18px 22px}.p1 .score>div{padding:10px 16px}
.p1 .lead{font-size:20px;margin:16px 0 22px}
.how{margin:22px 0 0;padding:0 0 0 22px;color:#c4cad8;font-size:17px}
.how li{margin-bottom:8px}
.how b{color:#fff}
.side{background:#0d0e12;border:1px solid #23262f;border-radius:14px;padding:44px 22px;margin-top:18px}
.side .t2{display:table;width:100%}
.side .t2>div{display:table-cell;vertical-align:middle}
.side .qr{width:150px;height:150px}
.side b.h{display:block;font-family:"Barlow Condensed","Arial Narrow",Arial,sans-serif;text-transform:uppercase;font-size:24px;margin-bottom:4px}
.side p{margin:0;color:#9aa1b3;font-size:15px}
.page2{padding-top:0;padding-bottom:14px}.page2 h2{font-size:34px;margin:2px 0 12px}.page2 .score{margin-bottom:14px}.page2 .score b{font-size:40px}.page2 .score>div{padding:12px 20px}.page2 .cd{padding:12px 18px}.page2 .cd p{font-size:14px;margin:4px 0}.page2 .cd em{display:none}.page2 .cd h3{font-size:24px}
@media (max-width:860px){
 .nav .w,.nav .w>div,.p1,.p1>div{display:block}
 .p1>div.l{width:auto;padding-right:0;margin-bottom:22px}
 .p1 h1{font-size:44px}
 .tabs{text-align:left;margin-top:10px}.tab{margin:0 8px 0 0}
}
</style>
</head>
<body>

<div class="nav"><div class="w">
 <div style="width:80px"><img class="lg" alt="JK Patchs" src="data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAASwAAAEsCAMAAABOo35HAAABgFBMVEVbXV1bm6KlkGPk06Fkz9wkXVqhopma4Ofh5tmZbV9eqs8wlaXWsJHPpWgiICJVmWbq1VwfZJtqTzRRdI8nNlEwosqOZzdny6pXLioCETKas8ohTDeqkiwFGGMpb6UOanI1hmbRrDOkyq4DFi8DFi6bcZ1UOUrvzTobZswrbJUsbZnGdl5rki8A//8AAP8yl6lLo9QNMlJYpqyvyl5SXKsNMVNRpNF06fA1hao1harMn9NbpdMVT20WU3Jrbm4POohpy+QfJ7cTdes0ndJuzul4eP+HJjI6gsL///8AcQAA/wA/weGEPUeV6/w7gsJEfcdNmLVQoLqbtOua+f//AP9VqlVown+9fMMAAAAFFjALJ0kEBxUURmwQNlUCCyX5+/QOIzcSOmYxh60rZ44UVHcpVnMtKCsSFBclSW4tdpWwl25uWEoTWYVx1+wVZo1MOTGRdlJQp8sGHEJz5fJ0ZlE0NTKLaE0pWYhMlrJSttD22U2L6PUwRlLIqG5SRTcwealAlWclAAAAgHRSTlP//v///v7//v///v7///v///z//v3+////EP///woPCf///55e/////mCh//8BARShXwf/BppbDJZe/x1gowP/lwMDBFgC/60BAgH//1xp/1ClCw0BA///AP7+//7+/v/+/v7+/v7///7+///+/v7///7+/v////7+/v/+/////o3pfT0AAIz1SURBVHja7b0JY9vmuSYKQAQoLqLMRbIseYmdxmnTLG2Sns70dDn7nNnnztzFAAkG3ElRJEWKlMiIIv/6fZ73+wCCkuzYiZO2Z+ZLbGsjBTx499V49X/OWx/j/0Dw1wfWZ589+/LLp//4j//4NHbw6RdffvnlZ5/9H7D0efYlYHn29NkbfuIfnuFHvnz2vzVYnz377T//a/TZ06df/Huefxcd+fSLL77YvOKfv/zt5//bgfXs2dOvFW999vSLLwhQJlN/7clkCNwXX4TU99sv/uHZ5/97gAWgngolgWaIUv2tD0ADZk+/5Kv/4enTrz//tw3W588UUz19+ot//y4wbUEmiKl3+YkJ7CcE67Ovnz1VQH3yybr+ww74UgB7+sWzz/4NgvWl3BWB6gGpdf2HwlWv5z/5xVORel/+2wLr2R9fKaTyPQAl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"></div>
 <div><span class="eb">PES 2021 &middot; PS4 &middot; GoldHEN</span></div>
 
</div></div>

<!-- ===== PARTE 1: INSTALAR ===== -->
<div id="p1">
 <div class="w p1">
  <div class="l">
   <h1>Patches, narradores<br>e placares num s&oacute; app</h1>
   <p class="lead">Instale o app uma vez e baixe tudo pelo controle, j&aacute; na pasta certa do jogo. Sem pendrive e sem copiar arquivo.</p>
   <a class="btn g" id="inst" href="#" onclick="instalarPS4();return false;">INSTALAR DIRETO NO PS4</a>
   <p class="st" id="inst-st">Abra esta p&aacute;gina no navegador do PS4 e toque no bot&atilde;o verde.</p>
   <ol class="how">
    <li>No GoldHEN, deixe <b>Enable Payload Server</b> ativado.</li>
    <li>No navegador do PS4, abra esta p&aacute;gina e toque em <b>INSTALAR DIRETO NO PS4</b>.</li>
    <li>Acompanhe o download em <b>Notifica&ccedil;&otilde;es</b> e aguarde terminar.</li>
    <li>Abra o <b>JK PATCHS</b>, cadastre-se e use <b>1 hora gr&aacute;tis</b> para testar.</li>
   </ol>
  </div>
  <div>
   <video id="v" controls autoplay muted loop playsinline preload="auto" src="/video-app" style="margin-top:0"></video>
   <p class="st">Se o v&iacute;deo estiver sem som, toque nele para ativar.</p>
</div>
  </div>
 </div>

 <div class="w"><table class="p1b" cellspacing="0" cellpadding="0"><tr>
  <td class="bl"><div class="score">
   <div><b>16</b><span>Narradores</span></div>
   <div><b>10</b><span>Placares por narrador</span></div>
   <div><b>7</b><span>Patches completos</span></div>
  </div>
</td>
  <td class="br">   <div class="side"><div class="t2">
    <div style="width:170px"><img class="qr" alt="QR Code do WhatsApp do admin" src="data:image/png;base64,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"></div>
    <div><b class="h">Quer ativar o seu acesso?</b><p>Aponte a c&acirc;mera do celular para o QR Code e chame no WhatsApp. Planos: Mensal <b style="color:#fff">R$ 15</b> &middot; Semestral <b style="color:#fff">R$ 80</b> &middot; Anual <b style="color:#fff">R$ 120</b>.</p></div>
   </div></div>
</td>
 </tr></table></div>
</div>

<!-- ===== PARTE 2: A LOJA ===== -->
<div id="p2">
 <div class="w page2">
  <div class="cards">
   <div class="cd f"><div><i>JK-MOD</i><h3>Mod completo do JK</h3><p>Narrador, placares das emissoras e abertura exclusiva, prontos para o seu patch. Voc&ecirc; escolhe o narrador e o app instala tudo.</p><em>Cada narrador com todos os placares</em></div></div>
   <div class="cd"><div><i>Narradores</i><h3>Vozes para o jogo</h3><p>Narradores da TV e do streaming, com a locu&ccedil;&atilde;o no lugar certo do jogo.</p><em>16 narradores</em></div></div>
   <div class="cd"><div><i>Placares</i><h3>Placares das emissoras</h3><p>Brasileir&atilde;o, Copa do Brasil, Libertadores e mais, nas vers&otilde;es de cada emissora.</p><em>10 placares por narrador</em></div></div>
   <div class="cd"><div><i>Patches</i><h3>Patches completos</h3><p>Os principais patches de PES 2021 para PS4, com a base e a atualiza&ccedil;&atilde;o, e v&iacute;deo mostrando como ativar.</p><em>7 patches &middot; instala&ccedil;&atilde;o guiada</em></div></div>
   <div class="cd"><div><i>Plugins</i><h3>Plugins do jogo</h3><p>Plugins de campeonatos, gameplay e visual, instalados na pasta certa do jogo.</p><em>Marcados em amarelo no app</em></div></div>
   <div class="cd"><div><i>Apps</i><h3>Ferramentas do PS4</h3><p>Gerenciador de arquivos, de saves e de cheats, entre outros apps &uacute;teis para o console.</p><em>Marcados em verde no app</em></div></div>
  </div>
  <div style="margin-top:26px">
   <div class="win">
    <div class="bar">APPs e Plugins</div>
    <div class="cols">
     <div class="col"><div class="info"><b>APPs e Plugins</b>Apps em verde, plugins em amarelo. Todo plugin instala na pasta certa do jogo.</div></div>
     <div class="col"><h4 class="gh">Apps</h4><span class="chip g">Gerenciador de arquivos</span><span class="chip g">Gerenciador de saves</span><span class="chip g">Gerenciador de cheats</span><span class="chip g">Instalador de pacotes</span></div>
     <div class="col"><h4 class="yh">Plugins</h4><span class="chip y">Plugin de gameplay</span><span class="chip y">Plugin de campeonato</span><span class="chip y">Plugin de visual</span></div>
    </div>
   </div>
   <div class="legend" style="margin-top:12px"><span><i style="background:#35d07f"></i>App em PKG (verde)</span><span><i style="background:#f5c542"></i>Plugin (amarelo)</span>&nbsp; Para assinar ou renovar, aperte <b>OPTIONS</b> no app.</div>
  </div>
 </div>
</div>

<div class="foot">JK PATCHS &middot; feito para a comunidade PES do PS4</div>

<script>
function aba(n){
  document.getElementById("p1").style.display=n===1?"block":"none";
  document.getElementById("p2").style.display=n===2?"block":"none";
  document.getElementById("t1").className="tab"+(n===1?" on":"");
  document.getElementById("t2").className="tab"+(n===2?" on":"");
  try{window.scrollTo(0,0);}catch(e){}
}
if(location.hash==="#loja"){aba(2);}
</script>
<script>

function instMsg(t){var e=document.getElementById("inst-st");if(e)e.textContent=t;}
function instalarPS4(){
  if(!/PlayStation/i.test(navigator.userAgent)){instMsg("Abra esta pagina no navegador do PS4 para instalar.");return;}
  if(location.protocol!=="http:"){location.href="http://jkpatchs.com/baixar#instalar";return;}
  instMsg("Preparando...");
  var x=new XMLHttpRequest();
  x.open("GET","/q.bin",true);
  x.responseType="arraybuffer";
  x.onload=function(){
    if(x.status!==200){instMsg("Erro "+x.status+" ao preparar a instalacao.");return;}
    var y=new XMLHttpRequest();var done=false;
    function fim(m){if(!done){done=true;instMsg(m);}}
    y.open("POST","http://127.0.0.1:9090/",true);
    y.setRequestHeader("Content-Type","text/plain");
    y.onload=function(){fim("Enviado! Veja o download em Notificacoes.");};
    y.onerror=function(){fim("Enviado! Veja o download em Notificacoes. Se nada aparecer, confira se o Payload Server esta ativo no GoldHEN.");};
    y.timeout=8000;
    y.ontimeout=function(){fim("Enviado! Veja o download em Notificacoes. Se nada aparecer, confira se o Payload Server esta ativo no GoldHEN.");};
    try{y.send(x.response);}catch(e){fim("Falha ao enviar: "+e);}
  };
  x.onerror=function(){instMsg("Falha ao preparar a instalacao.");};
  x.send();
}
if(location.hash==="#instalar"&&/PlayStation/i.test(navigator.userAgent)&&location.protocol==="http:"){setTimeout(instalarPS4,300);}

</script>
<script>
(function(){var v=document.getElementById("v");if(!v)return;
function go(){try{v.muted=true;var p=v.play();if(p&&p.catch)p.catch(function(){});}catch(e){}}
go();v.addEventListener("canplay",go);
v.addEventListener("click",function(){v.muted=false;});})();

</script>
</body>
</html>
`;

function landingPage() {
  return new Response(LANDING_HTML, {
    status: 200,
    headers: {
      "Content-Type": "text/html; charset=utf-8",
      "Cache-Control": "public, max-age=300",
      "X-Content-Type-Options": "nosniff"
    }
  });
}


// ---------- VIDEO DA PAGINA (publico) ----------
// Ajuste a chave se o seu video estiver com outro nome no R2.
const VIDEO_KEY = "APP-PS4/APP.mp4";
async function videoApp(request, env) {
  if (!env.APP_BUCKET) return text(500, "ERRO\nAPP_BUCKET nao configurado");
  const rangeHdr = request.headers.get("Range");
  let object = request.method === "HEAD"
    ? await env.APP_BUCKET.head(VIDEO_KEY)
    : (rangeHdr ? await env.APP_BUCKET.get(VIDEO_KEY, { range: request.headers }) : await env.APP_BUCKET.get(VIDEO_KEY));
  if (!object) return Response.redirect("https://jkpatchs.s.gy/mp4", 302);
  const headers = new Headers();
  headers.set("Content-Type", "video/mp4");
  headers.set("Accept-Ranges", "bytes");
  headers.set("Cache-Control", "public, max-age=3600");
  let status = 200, length = object.size;
  if (rangeHdr && object.range && request.method !== "HEAD") {
    const r = object.range;
    const off = r.offset !== undefined ? r.offset : 0;
    length = r.length !== undefined ? r.length : object.size - off;
    headers.set("Content-Range", "bytes " + off + "-" + (off + length - 1) + "/" + object.size);
    status = 206;
  }
  headers.set("Content-Length", String(length));
  return new Response(request.method === "HEAD" ? null : object.body, { status, headers });
}

// ---------- PAINEL (/jkpainel17) ----------
// Login: usuario = secret PANEL_USER (padrao "admin"), senha = ADMIN_KEY.
const PANEL_R2_PUBLIC = "https://pub-c6518eb1477949a58d959f5a769703d1.r2.dev/";
const PANEL_PROTECTED = [];
// Arquivos que o painel pode substituir (alem dos .jkv), sempre com confirmacao na tela.
const PANEL_REPLACEABLE = ["APP-PS4/JKPATCHS.pkg", "APP-PS4/APP.mp4"];
const PANEL_PROTECTED_PREFIX = ["APP-PS4/ICONE/"];
const PANEL_HOURS = 12;
const PANEL_MAX_UPLOAD = 99 * 1024 * 1024;   // limite do Cloudflare Workers para o corpo de uma requisicao: 100 MB
const PANEL_HIST_MAX = 30;

function panelProtected(k) {
  return PANEL_PROTECTED.indexOf(k) >= 0 || PANEL_PROTECTED_PREFIX.some((p) => k.indexOf(p) === 0);
}

async function panelMakeToken(env) {
  const exp = Math.floor(Date.now() / 1000) + PANEL_HOURS * 3600;
  const payload = b64u(enc.encode(JSON.stringify({ pn: 1, exp })));
  const sig = b64u(await hmac(env.TOKEN_SECRET, "pn:" + payload));
  return payload + "." + sig;
}

async function panelAuth(request, env) {
  const h = request.headers.get("Authorization") || "";
  return await panelTokenOk(env, h.indexOf("Bearer ") === 0 ? h.slice(7) : "");
}

async function panelTokenOk(env, t) {
  try {
    const [payload, sig] = t.split(".");
    if (!payload || !sig) return false;
    const good = b64u(await hmac(env.TOKEN_SECRET, "pn:" + payload));
    if (!safeEqual(good, sig)) return false;
    const d = JSON.parse(new TextDecoder().decode(fromB64u(payload)));
    return !!(d && d.pn === 1 && d.exp > Math.floor(Date.now() / 1000));
  } catch (e) {
    return false;
  }
}

async function panelAudit(env, action, keys) {
  try {
    let a = [];
    try { a = JSON.parse((await env.DB.get("paudit")) || "[]"); } catch (e) {}
    a.unshift({ t: Date.now(), a: action, keys: keys.slice(0, 50) });
    await env.DB.put("paudit", JSON.stringify(a.slice(0, 200)));
  } catch (e) {}
}

function panelCatStats(txt) {
  let g = 0, b = 0;
  String(txt).split("\n").forEach((l) => {
    if (l.indexOf("GAME\t") === 0) g++;
    else if (l.indexOf("BTN\t") === 0) b++;
  });
  return { g, b };
}

function panelKeyOk(k) {
  return typeof k === "string" && k.length > 0 && k.length < 900 && k.charAt(0) !== "/" &&
    k.indexOf("..") < 0 && k.indexOf("\\") < 0 && !/[\u0000-\u001f]/.test(k);
}

async function panelLogin(request, env) {
  const ip = request.headers.get("CF-Connecting-IP") || "x";
  const rk = "plogin:" + ip;
  const n = parseInt((await env.DB.get(rk)) || "0", 10);
  if (n >= 6) return json(429, { erro: "Muitas tentativas. Aguarde 15 minutos." });
  let f = {};
  try { f = await request.json(); } catch (e) {}
  const user = String(f.user || "").trim().toLowerCase();
  const wantUser = String(env.PANEL_USER || "admin").trim().toLowerCase();
  const okU = safeEqual(user, wantUser);
  const okP = safeEqual(String(f.pass || ""), env.ADMIN_KEY);
  if (!(okU && okP)) {
    await env.DB.put(rk, String(n + 1), { expirationTtl: 900 });
    return json(401, { erro: "Usuário ou senha incorretos." });
  }
  await env.DB.delete(rk);
  return json(200, { token: await panelMakeToken(env) });
}

async function panelApi(request, env, url) {
  const p = url.pathname.replace("/jkpainel17/api/", "");
  const m = request.method;
  if (p === "login" && m === "POST") return await panelLogin(request, env);
  if (!(await panelAuth(request, env))) return json(401, { erro: "não autorizado" });

  if (p === "me") return json(200, { ok: true });

  if (p === "catalog" && m === "GET") {
    return json(200, { catalog: (await env.DB.get("catalog")) || "" });
  }

  if (p === "catalog" && m === "PUT") {
    let f = {};
    try { f = await request.json(); } catch (e) { return json(400, { erro: "JSON inválido" }); }
    const novo = String(f.catalog || "");
    const st = panelCatStats(novo);
    if (!st.g || !st.b) return json(400, { erro: "Catálogo sem cards/botões. Não salvei para não apagar o do app." });
    if (novo.length > 5 * 1024 * 1024) return json(400, { erro: "Catálogo grande demais." });
    const velho = (await env.DB.get("catalog")) || "";
    if (velho) {
      const so = panelCatStats(velho);
      if (st.b < so.b * 0.9 || st.g < so.g * 0.9) {
        return json(400, { erro: "O novo catálogo tem bem menos cards/botões que o atual (" + st.g + "/" + st.b + " contra " + so.g + "/" + so.b + "). O painel não aceita remoções em massa." });
      }
    }
    if (velho && velho !== novo) {
      const ts = String(Date.now());
      await env.DB.put("cathist:" + ts, velho);
      let idx = [];
      try { idx = JSON.parse((await env.DB.get("cathist:index")) || "[]"); } catch (e) {}
      const sv = panelCatStats(velho);
      idx.unshift({ ts, g: sv.g, b: sv.b });
      const out = idx.slice(PANEL_HIST_MAX);
      for (const o of out) await env.DB.delete("cathist:" + o.ts);
      await env.DB.put("cathist:index", JSON.stringify(idx.slice(0, PANEL_HIST_MAX)));
    }
    await env.DB.put("catalog", novo);
    await panelAudit(env, "catálogo salvo (" + st.g + " cards, " + st.b + " botões)", []);
    return json(200, { ok: true });
  }

  if (p === "catalog/history" && m === "GET") {
    let idx = [];
    try { idx = JSON.parse((await env.DB.get("cathist:index")) || "[]"); } catch (e) {}
    return json(200, { items: idx });
  }

  if (p === "catalog/version" && m === "GET") {
    const ts = String(url.searchParams.get("ts") || "");
    if (!/^\d{10,16}$/.test(ts)) return json(400, { erro: "versão inválida" });
    const v = await env.DB.get("cathist:" + ts);
    if (v === null) return json(404, { erro: "versão não encontrada" });
    return json(200, { catalog: v });
  }

  if (p === "audit" && m === "GET") {
    let a = [];
    try { a = JSON.parse((await env.DB.get("paudit")) || "[]"); } catch (e) {}
    return json(200, { items: a });
  }

  if (p === "r2/list" && m === "GET") {
    const prefix = String(url.searchParams.get("prefix") || "");
    if (prefix && !panelKeyOk(prefix)) return json(400, { erro: "pasta inválida" });
    const r = await env.APP_BUCKET.list({ prefix, delimiter: "/", limit: 1000 });
    return json(200, {
      folders: r.delimitedPrefixes || [],
      files: r.objects.map((o) => ({ key: o.key, size: o.size, uploaded: o.uploaded })),
      truncated: !!r.truncated
    });
  }

  if (p === "r2/pkginfo" && m === "GET") {
    const key = String(url.searchParams.get("key") || "");
    if (!panelKeyOk(key) || key.slice(-1) === "/") return json(400, { erro: "nome inválido" });
    try {
      const r = await panelPkgInfo(env.APP_BUCKET, key);
      if (r.status !== 200) return json(r.status, { erro: r.erro });
      return json(200, { content_id: r.content_id, size: r.size, type: r.type, title: r.title });
    } catch (e) {
      return json(500, { erro: "falha ao ler o PKG: " + (e && e.message ? e.message : e) });
    }
  }

  if (p === "r2/all" && m === "GET") {
    const files = [];
    let cursor, truncated = false;
    for (let i = 0; i < 30; i++) {
      const r = await env.APP_BUCKET.list({ limit: 1000, cursor });
      r.objects.forEach((o) => files.push({ k: o.key, s: o.size, u: o.uploaded }));
      if (!r.truncated) break;
      cursor = r.cursor;
      if (i === 29) truncated = true;
    }
    return json(200, { files, truncated });
  }

  if (p === "refresh" && m === "POST") return json(200, { token: await panelMakeToken(env) });

  // ---- envio em partes (arquivos grandes): start -> varias partes -> complete ----
  if (p.indexOf("r2/mpu/") === 0) {
    const key = String(url.searchParams.get("key") || "");
    if (!panelKeyOk(key) || key.slice(-1) === "/") return json(400, { erro: "nome/pasta inválidos" });
    if (panelProtected(key)) return json(403, { erro: "Esse caminho é protegido." });
    const uid = String(url.searchParams.get("uploadId") || "");
    if (p === "r2/mpu/start" && m === "POST") {
      if (url.searchParams.get("overwrite") !== "1" && (await env.APP_BUCKET.head(key))) {
        return json(409, { erro: "Já existe um arquivo com esse nome (confirme a substituição na tela)." });
      }
      const ct = url.searchParams.get("ct") || "application/octet-stream";
      const mp = await env.APP_BUCKET.createMultipartUpload(key, { httpMetadata: { contentType: ct } });
      return json(200, { ok: true, uploadId: mp.uploadId });
    }
    if (!uid) return json(400, { erro: "uploadId ausente" });
    const mp = env.APP_BUCKET.resumeMultipartUpload(key, uid);
    if (p === "r2/mpu/part" && m === "PUT") {
      const n = parseInt(url.searchParams.get("n") || "0", 10);
      if (!(n >= 1 && n <= 10000)) return json(400, { erro: "número da parte inválido" });
      const len = parseInt(request.headers.get("Content-Length") || "0", 10);
      if (!len) return json(400, { erro: "parte vazia" });
      if (len > PANEL_MAX_UPLOAD) return json(413, { erro: "Parte maior que 99 MB." });
      const part = await mp.uploadPart(n, request.body);
      return json(200, { ok: true, n: part.partNumber, etag: part.etag });
    }
    if (p === "r2/mpu/complete" && m === "POST") {
      let f = {};
      try { f = await request.json(); } catch (e) { return json(400, { erro: "JSON inválido" }); }
      const parts = (Array.isArray(f.parts) ? f.parts : []).map((x) => ({ partNumber: x.partNumber | 0, etag: String(x.etag || "") }))
        .sort((a, b) => a.partNumber - b.partNumber);
      if (!parts.length) return json(400, { erro: "nenhuma parte enviada" });
      try { await mp.complete(parts); } catch (e) { return json(400, { erro: "Não consegui juntar as partes: " + (e && e.message ? e.message : e) }); }
      await panelAudit(env, (url.searchParams.get("overwrite") === "1" ? "substituído" : "enviado") + " (em partes, " + parts.length + ")", [key]);
      return json(200, { ok: true, key });
    }
    if (p === "r2/mpu/abort" && m === "POST") {
      try { await mp.abort(); } catch (e) {}
      return json(200, { ok: true });
    }
    return json(404, { erro: "rota inexistente" });
  }

  if (p === "r2/upload" && m === "PUT") {
    const key = String(url.searchParams.get("key") || "");
    if (!panelKeyOk(key) || key.slice(-1) === "/") return json(400, { erro: "nome/pasta inválidos" });
    if (panelProtected(key)) return json(403, { erro: "Esse caminho é protegido." });
    const len = parseInt(request.headers.get("Content-Length") || "0", 10);
    if (!len) return json(400, { erro: "arquivo vazio" });
    if (len > PANEL_MAX_UPLOAD) return json(413, { erro: "Arquivo maior que 99 MB." });
    const podeSubstituir = url.searchParams.get("overwrite") === "1";   // a tela sempre pede confirmacao antes
    if (!podeSubstituir && (await env.APP_BUCKET.head(key))) {
      return json(409, { erro: "Já existe um arquivo com esse nome (confirme a substituição na tela)." });
    }
    const ct = request.headers.get("Content-Type") || "application/octet-stream";
    await env.APP_BUCKET.put(key, request.body, { httpMetadata: { contentType: ct } });
    await panelAudit(env, url.searchParams.get("overwrite") === "1" ? "substituído" : "enviado", [key]);
    return json(200, { ok: true, key });
  }

  return json(404, { erro: "rota inexistente" });
}

function panelLogoResponse() {
  const b = atob(PANEL_LOGO.split(",")[1]);
  const u = new Uint8Array(b.length);
  for (let i = 0; i < b.length; i++) u[i] = b.charCodeAt(i);
  return new Response(u, { headers: { "Content-Type": "image/png", "Cache-Control": "public, max-age=86400" } });
}

const PANEL_LOGO = "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAASwAAAEsCAMAAABOo35HAAABgFBMVEVbXV1bm6KlkGPk06Fkz9wkXVqhopma4Ofh5tmZbV9eqs8wlaXWsJHPpWgiICJVmWbq1VwfZJtqTzRRdI8nNlEwosqOZzdny6pXLioCETKas8ohTDeqkiwFGGMpb6UOanI1hmbRrDOkyq4DFi8DFi6bcZ1UOUrvzTobZswrbJUsbZnGdl5rki8A//8AAP8yl6lLo9QNMlJYpqyvyl5SXKsNMVNRpNF06fA1hao1harMn9NbpdMVT20WU3Jrbm4POohpy+QfJ7cTdes0ndJuzul4eP+HJjI6gsL///8AcQAA/wA/weGEPUeV6/w7gsJEfcdNmLVQoLqbtOua+f//AP9VqlVown+9fMMAAAAFFjALJ0kEBxUURmwQNlUCCyX5+/QOIzcSOmYxh60rZ44UVHcpVnMtKCsSFBclSW4tdpWwl25uWEoTWYVx1+wVZo1MOTGRdlJQp8sGHEJz5fJ0ZlE0NTKLaE0pWYhMlrJSttD22U2L6PUwRlLIqG5SRTcwealAlWclAAAAgHRSTlP//v///v7//v///v7///v///z//v3+////EP///woPCf///55e/////mCh//8BARShXwf/BppbDJZe/x1gowP/lwMDBFgC/60BAgH//1xp/1ClCw0BA///AP7+//7+/v/+/v7+/v7///7+///+/v7///7+/v////7+/v/+/////o3pfT0AAIz1SURBVHja7b0JY9vmuSYKQAQoLqLMRbIseYmdxmnTLG2Sns70dDn7nNnnztzFAAkG3ElRJEWKlMiIIv/6fZ73+wCCkuzYiZO2Z+ZLbGsjBTx499V49X/OWx/j/0Dw1wfWZ589+/LLp//4j//4NHbw6RdffvnlZ5/9H7D0efYlYHn29NkbfuIfnuFHvnz2vzVYnz377T//a/TZ06df/Huefxcd+fSLL77YvOKfv/zt5//bgfXs2dOvFW999vSLLwhQJlN/7clkCNwXX4TU99sv/uHZ5/97gAWgngolgWaIUv2tD0ADZk+/5Kv/4enTrz//tw3W588UUz19+ot//y4wbUEmiKl3+YkJ7CcE67Ovnz1VQH3yybr+ww74UgB7+sWzz/4NgvWl3BWB6gGpdf2HwlWv5z/5xVORel/+2wLr2R9fKaTyPQAl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";
// PKGINFO-INICIO
// Le so o cabecalho de um PKG de PS4 no R2 (poucos KB): content_id, tamanho real e tipo (PS4GD/PS4GP/...).
async function panelPkgInfo(bucket, key) {
  const head = await bucket.head(key);
  if (!head) return { status: 404, erro: "arquivo não encontrado no R2" };
  const rb = async (off, len) => {
    const o = await bucket.get(key, { range: { offset: off, length: len } });
    return o ? new Uint8Array(await o.arrayBuffer()) : null;
  };
  const h = await rb(0, 0x1000);
  if (!h || h.length < 0x440 || h[0] !== 0x7f || h[1] !== 0x43 || h[2] !== 0x4e || h[3] !== 0x54)
    return { status: 422, erro: "não parece um PKG de PS4 (cabeçalho diferente)" };
  const dv = new DataView(h.buffer, h.byteOffset, h.byteLength);
  let contentId = "";
  for (let i = 0x40; i < 0x64 && h[i]; i++) contentId += String.fromCharCode(h[i]);
  const ec = dv.getUint32(0x10), eo = dv.getUint32(0x18);
  let category = "", title = "";
  if (ec > 0 && ec <= 256) {
    const tb = await rb(eo, ec * 32);
    if (tb) {
      const tv = new DataView(tb.buffer, tb.byteOffset, tb.byteLength);
      for (let i = 0; i < ec && (i + 1) * 32 <= tb.length; i++) {
        if (tv.getUint32(i * 32) !== 0x1000) continue;
        const off = tv.getUint32(i * 32 + 16), sz = tv.getUint32(i * 32 + 20);
        if (sz < 64 || sz > 16384) break;
        const sfo = await rb(off, sz);
        if (!sfo || sfo.length < 20 || sfo[1] !== 0x50 || sfo[2] !== 0x53 || sfo[3] !== 0x46) break;
        const sv = new DataView(sfo.buffer, sfo.byteOffset, sfo.byteLength);
        const kt = sv.getUint32(8, true), dt = sv.getUint32(12, true), n = sv.getUint32(16, true);
        for (let k = 0; k < n && k < 64; k++) {
          const ko = sv.getUint16(20 + k * 16, true), len = sv.getUint32(20 + k * 16 + 4, true), dof = sv.getUint32(20 + k * 16 + 12, true);
          let name = "";
          for (let q = kt + ko; q < sfo.length && sfo[q]; q++) name += String.fromCharCode(sfo[q]);
          let val = "";
          for (let q = dt + dof; q < dt + dof + len && q < sfo.length && sfo[q]; q++) val += String.fromCharCode(sfo[q]);
          if (name === "CATEGORY") category = val;
          if (name === "TITLE") title = val;
        }
        break;
      }
    }
  }
  return { status: 200, content_id: contentId, size: head.size, type: category ? "PS4" + category.toUpperCase() : "PS4GD", title };
}
// PKGINFO-FIM

const PANEL_HTML = "<!DOCTYPE html>\n<html lang=\"pt-BR\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n<title>JK PATCHS - Painel</title>\n<link rel=\"stylesheet\" href=\"https://fonts.googleapis.com/css2?family=Poppins:wght@600;700;800&display=swap\">\n<style>\n*{box-sizing:border-box}\nbody{margin:0;background:#000;color:#e8ecf5;font:14px Arial,Helvetica,sans-serif}\na{color:#4d9bff}\nbutton,input,select,textarea{font:inherit;color:#e8ecf5;background:#0d0e12;border:1px solid #2a2e3a;border-radius:8px;padding:7px 10px}\nbutton{cursor:pointer;background:#16181f}\nbutton:hover{border-color:#4d9bff}\nbutton.p{background:#35d07f;color:#04210f;border-color:#35d07f;font-weight:bold}\nbutton.d{border-color:#7a2a2a;color:#ff8a8a}\nbutton.s{padding:3px 8px;font-size:12px}\nbutton:disabled{opacity:.45;cursor:default}\ninput:focus,select:focus,textarea:focus{outline:none;border-color:#4d9bff}\n.mut{color:#8c93a6}.ok{color:#35d07f}.bad{color:#ff6b6b}.warn{color:#f5c542}\n[hidden]{display:none!important}\n#login{min-height:100vh;display:flex;align-items:center;justify-content:center}\n#login .box{width:340px;text-align:center}\n#login img{width:150px;height:150px;border-radius:50%;margin-bottom:14px}\n#login input{width:100%;margin-bottom:10px;padding:11px}\n#login button{width:100%;padding:11px}\n.top{display:flex;align-items:center;gap:14px;padding:10px 18px;border-bottom:1px solid #23262f;position:sticky;top:0;background:#000;z-index:5}\n.top img{width:42px;height:42px;border-radius:50%}\n.top b{font-size:17px}\n.tabs{display:flex;gap:6px;flex:1;margin-left:14px;flex-wrap:wrap}\n.tab{padding:8px 16px;border-radius:8px;background:#0d0e12;border:1px solid #23262f;cursor:pointer;color:#9aa1b3}\n.tab.on{background:#0b1830;border-color:#4d9bff;color:#fff}\n.page{padding:16px 18px}\nh2{margin:0 0 10px;font-size:18px;color:#35d07f}\n.bar{display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin-bottom:12px}\n.bar .sp{flex:1}\n.box{background:#0d0e12;border:1px solid #23262f;border-radius:12px;padding:12px}\ntable{border-collapse:collapse;width:100%}\nth,td{text-align:left;padding:7px 8px;border-bottom:1px solid #1b1e27;vertical-align:middle}\nth{color:#8c93a6;font-weight:normal;font-size:12px}\ntr:hover td{background:#0f1218}\n.ed{display:flex;gap:14px;align-items:flex-start}\n.ed .l{width:330px;flex:none;max-height:calc(100vh - 150px);overflow:auto}\n.ed .r{flex:1;min-width:0}\n.cd{display:flex;gap:9px;align-items:center;padding:8px;border:1px solid #23262f;border-radius:10px;margin-bottom:6px;cursor:pointer;background:#0d0e12}\n.cd.on{border-color:#4d9bff;background:#0b1830}\n.cd img{width:36px;height:61px;object-fit:contain;border-radius:5px;background:#16181f;flex:none}\n.cd .t{min-width:0}.cd .t b{display:block;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}\n.cd .t span{font-size:12px;color:#8c93a6}\n.fg{display:grid;grid-template-columns:repeat(2,1fr);gap:8px}\n.fg label{display:block;font-size:12px;color:#8c93a6}\n.fg input,.fg select,.fg textarea{width:100%}\n.fg .w{grid-column:1/3}\n.bt{border:1px solid #23262f;border-radius:10px;padding:9px;margin-bottom:8px;background:#0b0c10}\n.bt.bad{border-color:#7a2a2a}.bt.warn{border-color:#6b5a1d}\n.bt .r1{display:flex;gap:6px;align-items:center;flex-wrap:wrap;margin-bottom:6px}\n.bt .r1 input.lb{flex:1;min-width:160px;font-weight:bold}\n.bt .r2{display:grid;grid-template-columns:repeat(4,1fr);gap:6px}\n.bt .r2 .w2{grid-column:span 2}.bt .r2 .w4{grid-column:span 4}\n.bt .r2 input,.bt .r2 select,.bt .r2 textarea{width:100%;font-size:12px;padding:5px 7px}\n.bt small{color:#8c93a6;font-size:11px;display:block}\n.pill{display:inline-block;padding:1px 8px;border-radius:99px;font-size:11px;border:1px solid #2a2e3a;color:#9aa1b3}\n.pill.g{border-color:#1d6b43;color:#35d07f}.pill.r{border-color:#7a2a2a;color:#ff8a8a}.pill.y{border-color:#6b5a1d;color:#f5c542}\n.modal{position:fixed;inset:0;background:rgba(0,0,0,.75);z-index:20;display:flex;align-items:center;justify-content:center}\n.modal .m{background:#0d0e12;border:1px solid #2a2e3a;border-radius:14px;width:min(980px,94vw);max-height:90vh;display:flex;flex-direction:column}\n.modal .mh{padding:12px 16px;border-bottom:1px solid #23262f;display:flex;align-items:center;gap:10px}\n.modal .mh b{flex:1;font-size:16px}\n.modal .mb{padding:14px 16px;overflow:auto}\n.grid{display:flex;flex-wrap:wrap;gap:12px}\n.pc{width:176px;background:#0d0e12;border:1px solid #23262f;border-radius:12px;overflow:hidden;cursor:pointer}\n.pc img{width:100%;height:auto;aspect-ratio:744/1254;object-fit:contain;background:#16181f;display:block}\n.pc div{padding:9px}.pc small{color:#8c93a6;display:block;font-size:11px}\n.pc b{display:block;margin:3px 0}\n.chips{display:flex;gap:6px;flex-wrap:wrap;margin-bottom:12px}\n.chip{padding:6px 12px;border-radius:99px;border:1px solid #23262f;cursor:pointer;color:#9aa1b3;background:#0d0e12}\n.chip.on{border-color:#4d9bff;color:#fff;background:#0b1830}\n.crumb a{cursor:pointer;text-decoration:none}\n.th{width:34px;height:34px;object-fit:cover;border-radius:5px;vertical-align:middle;background:#16181f}\niframe{width:100%;height:calc(100vh - 90px);border:1px solid #23262f;border-radius:12px;background:#000}\ntextarea.raw{width:100%;height:calc(100vh - 220px);font:12px/1.4 Consolas,monospace}\n</style></head>\n<body>\n\n<div id=\"login\" hidden><div class=\"box\">\n  <img src=\"/jkpainel17/logo.png\" alt=\"JK Patchs\">\n  <h2 style=\"text-align:center\">Painel JK PATCHS</h2>\n  <input id=\"lu\" placeholder=\"Usu\u00e1rio\" autocomplete=\"username\">\n  <input id=\"lp\" type=\"password\" placeholder=\"Senha\" autocomplete=\"current-password\" onkeydown=\"if(event.key==='Enter')entrar()\">\n  <button class=\"p\" onclick=\"entrar()\">Entrar</button>\n  <p id=\"lm\" class=\"bad\"></p>\n</div></div>\n\n<div id=\"app\" hidden>\n <div class=\"top\">\n  <img src=\"/jkpainel17/logo.png\" alt=\"\"><b>JK PATCHS</b> <span class=\"mut\" style=\"font-size:11px\">painel v15</span>\n  <div class=\"tabs\" id=\"tabs\"></div>\n  <span id=\"dirtyTag\" class=\"warn\" hidden>\u25cf altera\u00e7\u00f5es n\u00e3o salvas</span>\n  <button class=\"s\" onclick=\"sair()\">Sair</button>\n </div>\n <div id=\"v_cat\" class=\"page\" hidden></div>\n <div id=\"v_r2\" class=\"page\" hidden></div>\n <div id=\"v_prev\" class=\"page\" hidden></div>\n  <div id=\"v_users\" class=\"page\" hidden></div>\n</div>\n\n<div id=\"modal\" class=\"modal\" hidden><div class=\"m\"><div class=\"mh\"><b id=\"mt\"></b><button class=\"s\" onclick=\"fechar()\">Fechar</button></div><div class=\"mb\" id=\"mb\"></div></div></div>\n\n<script>\n/*CATLIB*/\nfunction splitL(t){return String(t).replace(/\\r/g,\"\").split(\"\\n\")}\nfunction padTo(a,n){a=a.slice();while(a.length<n)a.push(\"\");return a}\nfunction parseCat(txt){\n  var m={head:[],cats:[],games:[],tail:[],endNL:/\\n$/.test(txt)};\n  var lines=splitL(txt);if(m.endNL)lines.pop();\n  for(var i=0;i<lines.length;i++){\n    var l=lines[i],f=l.split(\"\\t\");\n    if(f[0]===\"CAT\"){m.cats.push([f[1]||\"\",f[2]||\"\"])}\n    else if(f[0]===\"GAME\"){m.games.push({f:padTo(f.slice(1),7),btns:[]})}\n    else if(f[0]===\"BTN\"&&m.games.length){m.games[m.games.length-1].btns.push({f:padTo(f.slice(1),10),n:f.length-1})}\n    else if(l.charAt(0)===\"#\"&&!m.games.length&&!m.cats.length){m.head.push(l)}\n    else if(l!==\"\"){m.tail.push(l)}\n  }\n  return m;\n}\nfunction serCat(m){\n  var o=m.head.slice();\n  m.cats.forEach(function(c){o.push(\"CAT\\t\"+c[0]+\"\\t\"+c[1])});\n  m.games.forEach(function(g){\n    o.push(\"GAME\\t\"+g.f.join(\"\\t\"));\n    g.btns.forEach(function(b){o.push(\"BTN\\t\"+b.f.join(\"\\t\"))});\n  });\n  m.tail.forEach(function(l){o.push(l)});\n  return o.join(\"\\n\")+(m.endNL?\"\\n\":\"\");\n}\n/*ENDCATLIB*/\n\nvar R2B=\"https://pub-c6518eb1477949a58d959f5a769703d1.r2.dev/\";\nvar TK=\"\",PROT=[];\nvar M=null,ORIG=\"\",SEL=0,DIRTY=false,ALL=null,RAW=false;\nfunction $(i){return document.getElementById(i)}\nfunction esc(s){return String(s==null?\"\":s).replace(/[&<>\"]/g,function(c){return{\"&\":\"&amp;\",\"<\":\"&lt;\",\">\":\"&gt;\",'\"':\"&quot;\"}[c]})}\nfunction fs(n){n=+n||0;if(n>=1073741824)return (n/1073741824).toFixed(2)+\" GB\";if(n>=1048576)return (n/1048576).toFixed(1)+\" MB\";if(n>=1024)return (n/1024).toFixed(1)+\" KB\";return n+\" B\"}\nfunction fd(t){return t?new Date(t).toLocaleString(\"pt-BR\",{day:\"2-digit\",month:\"2-digit\",year:\"numeric\",hour:\"2-digit\",minute:\"2-digit\"}):\"-\"}\nfunction urlOf(k){return R2B+k.split(\"/\").map(encodeURIComponent).join(\"/\")}\nfunction keyOf(u){u=String(u||\"\").trim();if(u.indexOf(R2B)!==0)return null;var r=u.slice(R2B.length).split(\"?\")[0];try{return decodeURIComponent(r)}catch(e){return r}}\n\nasync function api(p,method,body){\n  var h={\"Authorization\":\"Bearer \"+TK};\n  var o={method:method||\"GET\",headers:h};\n  if(body!==undefined){h[\"Content-Type\"]=\"application/json\";o.body=JSON.stringify(body)}\n  var r=await fetch(\"/jkpainel17/api/\"+p,o);\n  var j={};try{j=await r.json()}catch(e){}\n  if(r.status===401&&p!==\"login\"){mostrarLogin(\"Sess\u00e3o expirada. Entre de novo.\");throw new Error(\"sess\u00e3o expirada\")}\n  if(!r.ok)throw new Error(j.erro||(\"erro \"+r.status));\n  return j;\n}\nfunction guardar(k,v){try{localStorage.setItem(k,v)}catch(e){}}\nfunction ler(k){try{return localStorage.getItem(k)||\"\"}catch(e){return \"\"}}\n\n/* ---------- login ---------- */\nfunction mostrarLogin(msg){$(\"login\").hidden=false;$(\"app\").hidden=true;$(\"lm\").textContent=msg||\"\";TK=\"\";guardar(\"pn_tk\",\"\")}\nasync function entrar(){\n  $(\"lm\").textContent=\"\";\n  try{\n    var r=await fetch(\"/jkpainel17/api/login\",{method:\"POST\",headers:{\"Content-Type\":\"application/json\"},body:JSON.stringify({user:$(\"lu\").value,pass:$(\"lp\").value})});\n    var j=await r.json();if(!r.ok)throw new Error(j.erro||\"erro\");\n    TK=j.token;guardar(\"pn_tk\",TK);$(\"lp\").value=\"\";iniciar();\n  }catch(e){$(\"lm\").textContent=e.message}\n}\nfunction sair(){if(DIRTY&&!confirm(\"H\u00e1 altera\u00e7\u00f5es n\u00e3o salvas no cat\u00e1logo. Sair mesmo assim?\"))return;DIRTY=false;mostrarLogin(\"\")}\nwindow.addEventListener(\"beforeunload\",function(e){if(DIRTY){e.preventDefault();e.returnValue=\"\"}});\n\n/* ---------- abas ---------- */\nvar TABS=[[\"users\",\"Usu\u00e1rios\"],[\"cat\",\"Cat\u00e1logo\"],[\"r2\",\"Arquivos (R2)\"],[\"prev\",\"Pr\u00e9via do app\"]];\nvar TABV=\"users\";\nfunction abas(){\n  $(\"tabs\").innerHTML=TABS.map(function(t){return \"<div class='tab\"+(t[0]===TABV?\" on\":\"\")+\"' onclick='irAba(\\\"\"+t[0]+\"\\\")'>\"+t[1]+\"</div>\"}).join(\"\");\n  TABS.forEach(function(t){$(\"v_\"+t[0]).hidden=t[0]!==TABV});\n}\nfunction irAba(a){TABV=a;abas();if(a===\"cat\")desenharCat();if(a===\"r2\")r2Abrir(R2P);if(a===\"prev\")desenharPrev();if(a===\"users\"){var v=$(\"v_users\");if(!v.firstChild)v.innerHTML=\"<iframe src='/jkpainel17/usuarios'></iframe>\"}}\nasync function iniciar(){\n  $(\"login\").hidden=true;$(\"app\").hidden=false;abas();\n  try{await carregarCat();await carregarAll()}catch(e){}\n  irAba(TABV);\n}\nfunction marcar(d){DIRTY=d;$(\"dirtyTag\").hidden=!d}\n\n/* ---------- modal ---------- */\nfunction abrir(t,h){$(\"mt\").textContent=t;$(\"mb\").innerHTML=h;$(\"modal\").hidden=false}\nfunction fechar(){$(\"modal\").hidden=true;$(\"mb\").innerHTML=\"\"}\n\n/* ---------- dados ---------- */\nasync function carregarCat(){var j=await api(\"catalog\");ORIG=j.catalog||\"\";M=parseCat(ORIG);if(SEL>=M.games.length)SEL=0;marcar(false)}\nasync function carregarAll(){var j=await api(\"r2/all\");ALL={};j.files.forEach(function(f){ALL[f.k]=f});ALL._trunc=j.truncated;ALL._n=j.files.length}\nfunction refKeys(){\n  var s={};if(!M)return s;\n  function add(u){var k=keyOf(u);if(k)s[k]=1}\n  M.games.forEach(function(g){add(g.f[1]);add(g.f[6]);g.btns.forEach(function(b){add(b.f[2]);String(b.f[9]||\"\").split(\"|\").forEach(add)})});\n  return s;\n}\n\n/* ---------- ABA CATALOGO ---------- */\nfunction desenharCat(){\n  var v=$(\"v_cat\");\n  if(!M){v.innerHTML=\"<p class='mut'>Carregando...</p>\";return}\n  var bar=\"<div class='bar'><h2 style='margin:0'>Cat\u00e1logo do app</h2><span class='mut'>\"+M.games.length+\" cards \u00b7 \"+M.games.reduce(function(a,g){return a+g.btns.length},0)+\" bot\u00f5es</span><span class='sp'></span>\"+\n   \"<button onclick='validarCat()'>Validar</button><button onclick='sincTam()'>Sincronizar tamanhos</button><button onclick='histCat()'>Hist\u00f3rico</button><button onclick='appAbrir()'>Atualiza\u00e7\u00e3o do app</button><button onclick='toggleRaw()'>\"+(RAW?\"Voltar ao editor\":\"Modo texto\")+\"</button>\"+\n   \"<button onclick='descartar()' \"+(DIRTY?\"\":\"disabled\")+\">Descartar</button><button class='p' onclick='salvarCat()' \"+(DIRTY?\"\":\"disabled\")+\">Salvar cat\u00e1logo</button></div>\";\n  if(RAW){v.innerHTML=bar+\"<textarea class='raw' id='rawta' spellcheck='false'></textarea><p class='mut'>Edite o texto e clique em &quot;Voltar ao editor&quot; para aplicar.</p>\";$(\"rawta\").value=serCat(M);return}\n  v.innerHTML=bar+\"<div class='ed'><div class='l' id='lcol'></div><div class='r' id='cardR'></div></div>\";\n  desenharLista();\n  desenharCard();\n}\nvar PATH=[],SQ=\"\";\nfunction catLabel(id){for(var i=0;i<M.cats.length;i++)if(M.cats[i][0]===id)return M.cats[i][1];return id}\nfunction norm(t){return String(t||\"\").toLowerCase().normalize(\"NFD\").replace(/[\u0300-\u036f]/g,\"\").replace(/[^a-z0-9]+/g,\" \").trim()}\nfunction childCat(g){\n  if(g.f[0]===\"hub\")return \"plugins\";\n  var t=norm(g.f[4]);\n  for(var i=0;i<M.cats.length;i++){var id=M.cats[i][0];if(id!==\"all\"&&id!==g.f[0]&&norm(M.cats[i][1])===t)return id}\n  return null;\n}\nfunction childSet(){var s={};M.games.forEach(function(g){var c=childCat(g);if(c)s[c]=1});return s}\nfunction nivelCards(){\n  var cur=PATH.length?PATH[PATH.length-1]:null,cs=childSet(),out=[];\n  M.games.forEach(function(g,i){if(cur===null?!cs[g.f[0]]:g.f[0]===cur)out.push(i)});\n  return out;\n}\nfunction cardRow(g,i){\n  var n=g.btns.length,ch=childCat(g),sub;\n  if(ch){var k=M.games.filter(function(x){return x.f[0]===ch}).length;sub=k+\" card(s) dentro\";}\n  else sub=esc(g.f[2])+\" \u00b7 \"+n+(n>32?\" \u26a0\":\"\")+\" bot\u00f5es\";\n  if(SQ)sub+=\" \u00b7 \"+esc(catLabel(g.f[0]));\n  return \"<div class='cd\"+(i===SEL?\" on\":\"\")+\"' onclick='\"+(ch&&!SQ?\"abrirPasta(\"+i+\")\":\"selCard(\"+i+\")\")+\"'><img src='\"+esc(g.f[1])+\"' onerror=\\\"this.style.visibility='hidden'\\\"><div class='t'><b>\"+esc(g.f[4]||\"(sem t\u00edtulo)\")+\"</b><span>\"+sub+\"</span></div>\"+(ch?\"<div style='margin-left:auto;font-size:22px;color:#8c93a6'>\u203a</div>\":\"\")+\"</div>\";\n}\nfunction desenharLista(){\n  var el=$(\"lcol\");if(!el)return;\n  var h=\"<div class='bar' style='margin-bottom:8px'><input id='sq' placeholder='buscar card...' value='\"+esc(SQ)+\"' oninput='SQ=this.value;desenharLista();var q=$(\\\"sq\\\");q.focus();q.setSelectionRange(q.value.length,q.value.length)' style='flex:1;min-width:0'></div>\";\n  var q=SQ.trim().toLowerCase();\n  if(q){\n    h+=\"<div class='mut' style='margin:4px 0 8px'>Resultado da busca</div>\";\n    var c=0;M.games.forEach(function(g,i){if((g.f[4]+\" \"+g.f[2]+\" \"+g.f[3]).toLowerCase().indexOf(q)>=0){h+=cardRow(g,i);c++}});\n    if(!c)h+=\"<p class='mut'>Nenhum card encontrado.</p>\";\n  }else{\n    var tr=[\"In\u00edcio\"];PATH.forEach(function(id){tr.push(catLabel(id))});\n    h+=\"<div class='bar' style='margin-bottom:8px'>\"+(PATH.length?\"<button class='s' onclick='voltarPasta()'>\u2190 Voltar</button>\":\"\")+\"<span class='mut' style='flex:1'>\"+esc(tr.join(\" \u203a \"))+\"</span><button class='s' onclick='novoCard()'>+ Novo card\"+(PATH.length?\" aqui\":\"\")+\"</button></div>\";\n    var l=nivelCards();\n    l.forEach(function(i){h+=cardRow(M.games[i],i)});\n    if(!l.length)h+=\"<p class='mut'>Nada aqui ainda. Use &quot;+ Novo card&quot;.</p>\";\n  }\n  el.innerHTML=h;\n}\nfunction abrirPasta(i){SEL=i;PATH.push(childCat(M.games[i]));desenharCat()}\nfunction voltarPasta(){PATH.pop();desenharCat()}\nfunction selCard(i){SEL=i;desenharCat()}\nfunction catOpts(sel){\n  var o=M.cats.map(function(c){return \"<option value='\"+esc(c[0])+\"'\"+(c[0]===sel?\" selected\":\"\")+\">\"+esc(c[1])+\" (\"+esc(c[0])+\")</option>\"}).join(\"\");\n  if(sel&&!M.cats.some(function(c){return c[0]===sel}))o+=\"<option value='\"+esc(sel)+\"' selected>\"+esc(sel)+\"</option>\";\n  return o;\n}\nfunction desenharCard(){\n  var g=M.games[SEL],el=$(\"cardR\");if(!g){el.innerHTML=\"<p class='mut'>Nenhum card.</p>\";return}\n  var h=\"<div class='box' style='margin-bottom:12px'><div class='bar'><b>Card</b><span class='sp'></span><button class='s' onclick='moveCard(-1)'>\u2191 subir</button><button class='s' onclick='moveCard(1)'>\u2193 descer</button></div><div class='fg'>\"+\n   \"<div><label>Categoria</label><select onchange='setG(0,this.value)'>\"+catOpts(g.f[0])+\"</select></div>\"+\n   \"<div><label>Etiqueta (tag)</label><input value='\"+esc(g.f[2])+\"' oninput='setG(2,this.value,1)'></div>\"+\n   \"<div class='w'><label>Capa (URL) <button class='s' onclick='pickCover()'>escolher do R2</button> <button class='s' onclick='capaAbrir()'>gerar capa</button></label><input value='\"+esc(g.f[1])+\"' oninput='setG(1,this.value,1)'></div>\"+\n   \"<div><label>T\u00edtulo</label><input value='\"+esc(g.f[4])+\"' oninput='setG(4,this.value,1)'></div>\"+\n   \"<div><label>Subt\u00edtulo (kicker)</label><input value='\"+esc(g.f[3])+\"' oninput='setG(3,this.value,1)'></div>\"+\n   \"<div class='w'><label>Descri\u00e7\u00e3o</label><textarea rows='3' oninput='setG(5,this.value,1)'>\"+esc(g.f[5])+\"</textarea></div>\"+\n   \"<div class='w'><label>V\u00eddeo do card (URL .jkv, opcional) <button class='s' onclick='jkvAbrir(-1)'>gerar v\u00eddeo</button></label><input value='\"+esc(g.f[6])+\"' oninput='setG(6,this.value,1)'></div></div></div>\";\n  h+=\"<div class='bar'><b>Bot\u00f5es (\"+g.btns.length+(g.btns.length>32?\" \u2014 passou do limite de 32!\":\"\")+\")</b><span class='sp'></span><button class='s' onclick='novoBtn(\\\"pkg\\\")'>+ PKG</button><button class='s' onclick='novoBtn(\\\"video\\\")'>+ V\u00eddeo</button><button class='s' onclick='novoBtn(\\\"image\\\")'>+ Imagem</button><button class='s' onclick='novoBtn(\\\"download\\\")'>+ Arquivo</button></div>\";\n  h+=g.btns.map(function(b,i){return btnHtml(g,b,i)}).join(\"\");\n  el.innerHTML=h;\n}\nfunction btnProb(b){\n  var f=b.f,a=f[7]||\"pkg\",k=keyOf(f[2]),p=[];\n  if(!f[0])p.push([\"bad\",\"sem texto\"]);\n  if(!f[2])p.push([\"bad\",\"sem URL\"]);\n  if(a===\"pkg\"&&!f[3])p.push([\"bad\",\"PKG sem content_id\"]);\n  if(a===\"pkg\"&&f[2]&&(!f[4]||f[4]===\"0\"))p.push([\"warn\",\"tamanho 0\"]);\n  if(ALL&&k&&!ALL[k])p.push([\"bad\",\"arquivo n\u00e3o existe no R2\"]);\n  if(ALL&&k&&ALL[k]&&a===\"pkg\"&&String(ALL[k].s)!==String(f[4]))p.push([\"warn\",\"tamanho diferente do R2\"]);\n  return p;\n}\nfunction btnHtml(g,b,i){\n  var f=b.f,a=f[7]||\"pkg\",pr=btnProb(b);\n  var cls=\"bt\"+(pr.some(function(x){return x[0]===\"bad\"})?\" bad\":(pr.length?\" warn\":\"\"));\n  var pills=pr.map(function(x){return \"<span class='pill \"+(x[0]===\"bad\"?\"r\":\"y\")+\"'>\"+esc(x[1])+\"</span>\"}).join(\" \");\n  var k=keyOf(f[2]);if(ALL&&k&&ALL[k])pills+=\" <span class='pill g'>\"+fs(ALL[k].s)+\"</span>\";\n  var h=\"<div class='\"+cls+\"'><div class='r1'><input class='lb' value='\"+esc(f[0])+\"' placeholder='Texto do bot\u00e3o' oninput='setB(\"+i+\",0,this.value)'>\"+\n   \"<select onchange='setB(\"+i+\",7,this.value,2)'>\"+[\"pkg\",\"video\",\"image\",\"download\"].map(function(x){return \"<option\"+(x===a?\" selected\":\"\")+\">\"+x+\"</option>\"}).join(\"\")+\"</select>\"+\n   \"<button class='s' onclick='moveBtn(\"+i+\",-1)'>\u2191</button><button class='s' onclick='moveBtn(\"+i+\",1)'>\u2193</button>\"+\n   \"<select onchange='moverBtn(\"+i+\",this.value)'><option value=''>mover para\u2026</option>\"+M.games.map(function(x,j){return j===SEL?\"\":\"<option value='\"+j+\"'>\"+esc(x.f[4])+\"</option>\"}).join(\"\")+\"</select>\"+\n   \"</div><div>\"+pills+\"</div><div class='r2' style='margin-top:6px'>\"+\n   \"<div class='w4'><small>Descri\u00e7\u00e3o (aparece no app)</small><input value='\"+esc(f[1])+\"' oninput='setB(\"+i+\",1,this.value)'></div>\"+\n   \"<div class='w4'><small>URL do arquivo <button class='s' onclick='pickUrl(\"+i+\")'>escolher do R2</button>\"+(a===\"video\"?\" <button class='s' onclick='jkvAbrir(\"+i+\")'>gerar v\u00eddeo</button>\":\"\")+\"</small><input value='\"+esc(f[2])+\"' oninput='setB(\"+i+\",2,this.value,3)' onchange='autoPkg(\"+i+\")'></div>\";\n  if(a===\"pkg\"){\n    h+=\"<div class='w2'><small>content_id <button class='s' onclick='lerPkg(\"+i+\")'>ler do PKG</button></small><input value='\"+esc(f[3])+\"' oninput='setB(\"+i+\",3,this.value,3)'></div>\"+\n       \"<div><small>tamanho (bytes)</small><input value='\"+esc(f[4])+\"' oninput='setB(\"+i+\",4,this.value,3)'></div>\"+\n       \"<div><small>tipo</small><select onchange='setB(\"+i+\",5,this.value)'>\"+[\"PS4GD\",\"PS4GP\",\"PS4AC\",\"PS4AL\",\"\"].map(function(x){return \"<option value='\"+x+\"'\"+(x===f[5]?\" selected\":\"\")+\">\"+(x||\"(vazio)\")+\"</option>\"}).join(\"\")+\"</select></div>\"+\n       \"<div><small>jogo base?</small><select onchange='setB(\"+i+\",6,this.value)'><option value='0'\"+(f[6]!==\"1\"?\" selected\":\"\")+\">n\u00e3o</option><option value='1'\"+(f[6]===\"1\"?\" selected\":\"\")+\">sim</option></select></div>\";\n  }\n  if(a===\"download\"){\n    h+=\"<div class='w2'><small>pasta de destino no PS4</small><input value='\"+esc(f[8])+\"' oninput='setB(\"+i+\",8,this.value)'></div>\"+\n       \"<div class='w4'><small>arquivos extras (URLs separadas por |)</small><textarea rows='2' oninput='setB(\"+i+\",9,this.value,3)'>\"+esc(f[9])+\"</textarea></div>\";\n  }\n  return h+\"</div></div>\";\n}\nfunction sujo(){marcar(true);var d=$(\"dirtyTag\");if(d)d.hidden=false;fixBar()}\nfunction setG(i,v,nr){M.games[SEL].f[i]=v;if(i===0&&!SQ)PATH=childSet()[v]?[v]:[];sujo();if(!nr)desenharCat();else{var cdv=document.querySelectorAll(\".cd\");if(cdv[SEL]){var t=cdv[SEL].querySelector(\"b\");if(t&&i===4)t.textContent=v;if(i===1)cdv[SEL].querySelector(\"img\").src=v}}fixBar()}\nfunction setB(i,j,v,mode){var b=M.games[SEL].btns[i];b.f[j]=v;sujo();if(mode===2){if(v===\"video\"||v===\"image\"){b.f[3]=\"\";b.f[4]=\"0\";b.f[5]=\"\";b.f[6]=\"0\"}if(v===\"pkg\"){b.f[5]=b.f[5]||\"PS4GD\"}desenharCat()}else if(mode===3){}fixBar()}\nfunction fixBar(){var bs=document.querySelectorAll(\"#v_cat .bar button\");bs.forEach(function(b){if(b.textContent===\"Salvar cat\u00e1logo\"||b.textContent===\"Descartar\")b.disabled=!DIRTY})}\nfunction novoCard(){var cat=PATH.length?PATH[PATH.length-1]:\"patch\";M.games.push({f:padTo([cat,\"\",\"\",\"\",\"Novo card\",\"\",\"\"],7),btns:[]});SEL=M.games.length-1;sujo();desenharCat()}\nfunction moveCard(d){var c=M.games[SEL].f[0],j=SEL+d;while(j>=0&&j<M.games.length&&M.games[j].f[0]!==c)j+=d;if(j<0||j>=M.games.length)return;var t=M.games[SEL];M.games[SEL]=M.games[j];M.games[j]=t;SEL=j;sujo();desenharCat()}\nfunction novoBtn(a){var f=[\"NOVO BOT\u00c3O\",\"\",\"\",\"\",\"0\",\"\",\"0\",a,\"\",\"\"];if(a===\"pkg\"){f[5]=\"PS4GD\"}M.games[SEL].btns.push({f:f,n:10});sujo();desenharCat()}\nfunction moveBtn(i,d){var a=M.games[SEL].btns,j=i+d;if(j<0||j>=a.length)return;var t=a[i];a[i]=a[j];a[j]=t;sujo();desenharCat()}\nfunction moverBtn(i,j){if(j===\"\")return;var a=M.games[SEL].btns;var b=a.splice(i,1)[0];M.games[+j].btns.push(b);sujo();desenharCat()}\nfunction toggleRaw(){\n  if(RAW){var t=$(\"rawta\").value;if(serCat(M)!==t){M=parseCat(t);if(SEL>=M.games.length)SEL=0;sujo()}RAW=false}else RAW=true;\n  desenharCat();\n}\nfunction descartar(){if(!confirm(\"Descartar as altera\u00e7\u00f5es e voltar ao cat\u00e1logo salvo?\"))return;M=parseCat(ORIG);SEL=0;marcar(false);desenharCat()}\n\nfunction problemas(){\n  var er=[],av=[];\n  M.games.forEach(function(g,gi){\n    var n=g.btns.length;\n    if(n>32)er.push(\"Card \\\"\"+g.f[4]+\"\\\" tem \"+n+\" bot\u00f5es (limite 32).\");\n    if(g.f[1]&&ALL){var k=keyOf(g.f[1]);if(k&&!ALL[k])av.push(\"Card \\\"\"+g.f[4]+\"\\\": capa n\u00e3o existe no R2.\")}\n    g.btns.forEach(function(b){\n      btnProb(b).forEach(function(p){(p[0]===\"bad\"?er:av).push(\"Card \\\"\"+g.f[4]+\"\\\" \u203a \\\"\"+b.f[0]+\"\\\": \"+p[1]+\".\")});\n    });\n  });\n  return {er:er,av:av};\n}\nfunction validarCat(){\n  var p=problemas();\n  var h=\"<p>\"+(ALL?\"Comparado com \"+ALL._n+\" arquivos do R2.\":\"Lista do R2 n\u00e3o carregada.\")+\"</p>\";\n  h+=p.er.length?\"<h3 class='bad'>Erros (\"+p.er.length+\")</h3><ul>\"+p.er.map(function(x){return \"<li>\"+esc(x)+\"</li>\"}).join(\"\")+\"</ul>\":\"<p class='ok'>Nenhum erro.</p>\";\n  h+=p.av.length?\"<h3 class='warn'>Avisos (\"+p.av.length+\")</h3><ul>\"+p.av.map(function(x){return \"<li>\"+esc(x)+\"</li>\"}).join(\"\")+\"</ul>\":\"<p class='ok'>Nenhum aviso.</p>\";\n  abrir(\"Valida\u00e7\u00e3o do cat\u00e1logo\",h);\n}\nfunction sincTam(){\n  if(!ALL){alert(\"Lista do R2 n\u00e3o carregada.\");return}\n  var n=0;M.games.forEach(function(g){g.btns.forEach(function(b){if((b.f[7]||\"pkg\")!==\"pkg\")return;var k=keyOf(b.f[2]);if(k&&ALL[k]&&String(ALL[k].s)!==String(b.f[4])){b.f[4]=String(ALL[k].s);n++}})});\n  if(n){sujo();desenharCat()}alert(n?n+\" tamanho(s) atualizado(s) com o valor real do R2. Falta salvar.\":\"Todos os tamanhos j\u00e1 batem com o R2.\");\n}\nasync function salvarCat(){\n  if(RAW){toggleRaw()}\n  var p=problemas();\n  if(p.er.length){abrir(\"N\u00e3o d\u00e1 para salvar\",\n    \"<p class='bad'>Corrija antes de salvar:</p><ul>\"+p.er.map(function(x){return \"<li>\"+esc(x)+\"</li>\"}).join(\"\")+\"</ul>\");return}\n  if(p.av.length&&!confirm(\"H\u00e1 \"+p.av.length+\" aviso(s) (arquivos que n\u00e3o existem no R2 ou tamanhos diferentes). Salvar mesmo assim?\"))return;\n  try{var txt=serCat(M);var j=await api(\"catalog\",\"PUT\",{catalog:txt});ORIG=txt;marcar(false);desenharCat();alert(\"Cat\u00e1logo salvo. O app j\u00e1 usa a vers\u00e3o nova (a anterior ficou no Hist\u00f3rico).\")}catch(e){alert(\"Erro ao salvar: \"+e.message)}\n}\nasync function histCat(){\n  try{var j=await api(\"catalog/history\");\n    var h=j.items.length?\"<table><tr><th>Data</th><th>Cards</th><th>Bot\u00f5es</th><th></th></tr>\"+j.items.map(function(x){return \"<tr><td>\"+fd(+x.ts)+\"</td><td>\"+(x.g||\"-\")+\"</td><td>\"+(x.b||\"-\")+\"</td><td><button class='s' onclick='restaurar(\\\"\"+x.ts+\"\\\")'>Restaurar esta vers\u00e3o</button></td></tr>\"}).join(\"\")+\"</table>\":\"<p class='mut'>Ainda n\u00e3o h\u00e1 vers\u00f5es anteriores. Elas aparecem a cada &quot;Salvar&quot;.</p>\";\n    abrir(\"Hist\u00f3rico do cat\u00e1logo (\u00faltimas \"+j.items.length+\")\",h+\"<p class='mut'>Restaurar s\u00f3 carrega a vers\u00e3o no editor. Falta clicar em &quot;Salvar cat\u00e1logo&quot;.</p>\");\n  }catch(e){alert(e.message)}\n}\nasync function restaurar(ts){\n  try{var j=await api(\"catalog/version?ts=\"+encodeURIComponent(ts));M=parseCat(j.catalog||\"\");SEL=0;RAW=false;sujo();fechar();desenharCat();alert(\"Vers\u00e3o carregada no editor. Revise e clique em Salvar cat\u00e1logo.\")}catch(e){alert(e.message)}\n}\n\n/* ---------- seletor de arquivo do R2 (modal) ---------- */\nvar PK=null,PKP=\"\";\nasync function pkAbrir(pref){\n  PKP=pref;\n  try{var j=await api(\"r2/list?prefix=\"+encodeURIComponent(pref));\n    var h=\"<div class='crumb mut' style='margin-bottom:8px'>\"+crumb(pref,\"pkAbrir\")+\"</div><table>\";\n    j.folders.forEach(function(d){h+=\"<tr><td colspan='3'><a onclick='pkAbrir(\\\"\"+esc(d)+\"\\\")'>\ud83d\udcc1 \"+esc(d.slice(pref.length))+\"</a></td></tr>\"});\n    j.files.forEach(function(f){h+=\"<tr><td><a style='cursor:pointer' onclick='pkEsc(\\\"\"+esc(f.key).replace(/'/g,\"&#39;\")+\"\\\")'>\"+esc(f.key.slice(pref.length))+\"</a></td><td>\"+fs(f.size)+\"</td><td class='mut'>\"+fd(f.uploaded)+\"</td></tr>\"});\n    h+=\"</table>\";\n    $(\"mb\").innerHTML=h;\n  }catch(e){$(\"mb\").innerHTML=\"<p class='bad'>\"+esc(e.message)+\"</p>\"}\n}\nfunction pickUrl(i){PK={t:\"btn\",i:i};abrir(\"Escolher arquivo do R2\",\"\");pkAbrir(\"\")}\nfunction pickCover(){PK={t:\"cover\"};abrir(\"Escolher capa do R2\",\"\");pkAbrir(\"COVERS/\")}\nfunction pkEsc(key){\n  if(PK.t===\"app\"){appAbrir();if(APPV){$(\"ap1\").value=APPV.a;$(\"ap5\").value=APPV.b;$(\"ap3\").value=APPV.c;$(\"ap4\").value=APPV.d}$(\"ap2\").value=urlOf(key);if(ALL&&ALL[key])$(\"ap4\").value=String(ALL[key].s);return}\n  var g=M.games[SEL];\n  if(PK.t===\"cover\"){g.f[1]=urlOf(key)}\n  var bi=-1;\n  if(PK.t!==\"cover\"){bi=PK.i;var b=g.btns[bi];b.f[2]=urlOf(key);if((b.f[7]||\"pkg\")===\"pkg\"&&ALL&&ALL[key])b.f[4]=String(ALL[key].s)}\n  sujo();fechar();desenharCat();\n  if(bi>=0&&(g.btns[bi].f[7]||\"pkg\")===\"pkg\")lerPkg(bi,true);\n}\nasync function lerPkg(i,quiet){\n  var b=M.games[SEL].btns[i],k=keyOf(b.f[2]);\n  if(!k){if(!quiet)alert(\"O link precisa ser de um arquivo do seu R2.\");return}\n  try{var j=await api(\"r2/pkginfo?key=\"+encodeURIComponent(k));\n    b.f[3]=j.content_id;b.f[4]=String(j.size);b.f[5]=j.type;sujo();desenharCat()}\n  catch(e){if(!quiet)alert(\"N\u00e3o consegui ler o PKG: \"+e.message)}\n}\nfunction autoPkg(i){var b=M.games[SEL].btns[i];if((b.f[7]||\"pkg\")===\"pkg\"&&/\\.pkg$/i.test(keyOf(b.f[2])||\"\"))lerPkg(i,true)}\nfunction crumb(pref,fn){\n  var parts=pref.split(\"/\").filter(Boolean),acc=\"\",h=\"<a onclick='\"+fn+\"(\\\"\\\")'>raiz</a>\";\n  parts.forEach(function(p){acc+=p+\"/\";h+=\" / <a onclick='\"+fn+\"(\\\"\"+esc(acc).replace(/'/g,\"&#39;\")+\"\\\")'>\"+esc(p)+\"</a>\"});\n  return h;\n}\n\n/* ---------- ABA R2 ---------- */\nvar R2P=\"\";\nasync function r2Abrir(pref){\n  R2P=pref;var v=$(\"v_r2\");\n  v.innerHTML=\"<p class='mut'>Carregando...</p>\";\n  try{\n    if(!ALL)await carregarAll();\n    var j=await api(\"r2/list?prefix=\"+encodeURIComponent(pref));\n    var ref=refKeys();\n    var h=\"<div class='bar'><h2 style='margin:0'>Arquivos no R2</h2><span class='mut'>\"+ALL._n+\" arquivos no bucket\"+(ALL._trunc?\" (lista parcial)\":\"\")+\"</span><span class='sp'></span><button onclick='r2Auditoria()'>Hist\u00f3rico de envios</button><button onclick='r2Recarregar()'>Atualizar</button><button class='p' onclick='r2Upload()'>Enviar arquivo</button></div>\";\n    h+=\"<div class='box'><div class='crumb' style='margin-bottom:8px'>\"+crumb(pref,\"r2Abrir\")+\"</div><table><tr><th>Nome</th><th>Tamanho</th><th>Data</th><th></th><th></th></tr>\";\n    j.folders.forEach(function(d){h+=\"<tr><td colspan='4'><a style='cursor:pointer' onclick='r2Abrir(\\\"\"+esc(d).replace(/'/g,\"&#39;\")+\"\\\")'>\ud83d\udcc1 \"+esc(d.slice(pref.length))+\"</a></td><td></td></tr>\"});\n    j.files.forEach(function(f,i){\n      var img=/\\.(png|jpe?g|webp|gif)$/i.test(f.key)&&f.size<4e6?\"<img class='th' loading='lazy' src='\"+esc(urlOf(f.key))+\"'> \":\"\";\n      var uso=ref[f.key]?\"<span class='pill g'>no cat\u00e1logo</span>\":(PROT.indexOf(f.key)>=0||f.key.indexOf(\"APP-PS4/ICONE/\")===0?\"<span class='pill y'>protegido</span>\":\"<span class='pill'>sem uso</span>\");\n      h+=\"<tr><td>\"+img+esc(f.key.slice(pref.length))+\"</td><td>\"+fs(f.size)+\"</td><td class='mut'>\"+fd(f.uploaded)+\"</td><td>\"+uso+\"</td><td style='white-space:nowrap'><button class='s' onclick='r2Copiar(\"+i+\")'>copiar link</button> <a href='\"+esc(urlOf(f.key))+\"' target='_blank'><button class='s'>abrir</button></a></td></tr>\";\n    });\n    if(!j.folders.length&&!j.files.length)h+=\"<tr><td colspan='5' class='mut'>Pasta vazia.</td></tr>\";\n    h+=\"</table>\"+(j.truncated?\"<p class='warn'>H\u00e1 mais itens nesta pasta do que o painel mostra.</p>\":\"\")+\"</div>\";\n    v.innerHTML=h;window.R2F=j.files;\n  }catch(e){v.innerHTML=\"<p class='bad'>\"+esc(e.message)+\"</p>\"}\n}\nasync function r2Recarregar(){ALL=null;await carregarAll();r2Abrir(R2P)}\nfunction r2Copiar(i){var u=urlOf(R2F[i].key);if(navigator.clipboard)navigator.clipboard.writeText(u).then(function(){alert(\"Link copiado\")},function(){prompt(\"Copie o link:\",u)});else prompt(\"Copie o link:\",u)}\nasync function r2Auditoria(){\n  try{var j=await api(\"audit\");abrir(\"Hist\u00f3rico de envios e salvamentos\",j.items.length?\"<table><tr><th>Quando</th><th>A\u00e7\u00e3o</th><th>Arquivos</th></tr>\"+j.items.map(function(x){return \"<tr><td>\"+fd(x.t)+\"</td><td>\"+esc(x.a)+\"</td><td>\"+esc((x.keys||[]).join(\", \"))+\"</td></tr>\"}).join(\"\")+\"</table>\":\"<p class='mut'>Nada registrado ainda.</p>\")}catch(e){alert(e.message)}\n}\nfunction r2Upload(){\n  abrir(\"Enviar arquivo para o R2\",\n   \"<p class='mut'>Qualquer tamanho (acima de 90 MB o envio \u00e9 feito em partes, com retomada). Se j\u00e1 existir um arquivo com o mesmo nome, o painel pergunta se pode substituir.</p>\"+\n   \"<div class='fg'><div class='w'><label>Pasta de destino</label><input id='upd' value='\"+esc(R2P)+\"'></div><div class='w'><label>Arquivo</label><input type='file' id='upf'></div><div class='w'><label>Nome no R2 (deixe vazio para manter)</label><input id='upn'></div></div>\"+\n   \"<p><button class='p' onclick='r2Enviar()'>Enviar</button> <span id='ups' class='mut'></span></p>\");\n}\nasync function r2Enviar(forcar){\n  var f=$(\"upf\").files[0];if(!f){alert(\"Escolha um arquivo.\");return}\n  if(!ALL){$(\"ups\").textContent=\"conferindo arquivos...\";try{await carregarAll()}catch(e){}}\n  var pasta=$(\"upd\").value.trim().replace(/^\\/+/,\"\");if(pasta&&pasta.slice(-1)!==\"/\")pasta+=\"/\";\n  var nome=($(\"upn\").value.trim()||f.name),key=pasta+nome;\n  var sobre=\"\";\n  if(forcar===true){sobre=\"&overwrite=1\"}\n  else if(ALL&&ALL[key]){\n    if(!confirm(\"J\u00e1 existe \"+key+\" (\"+(ALL[key].s/1048576).toFixed(1)+\" MB).\\nSubstituir pelo novo arquivo (\"+(f.size/1048576).toFixed(1)+\" MB)?\\nO antigo ser\u00e1 perdido.\"))return;\n    sobre=\"&overwrite=1\";\n  }\n  if(f.size>90*1048576){mpuEnviar(f,key,sobre);return}\n  var x=new XMLHttpRequest();x.open(\"PUT\",\"/jkpainel17/api/r2/upload?key=\"+encodeURIComponent(key)+sobre);\n  x.setRequestHeader(\"Authorization\",\"Bearer \"+TK);x.setRequestHeader(\"Content-Type\",f.type||\"application/octet-stream\");\n  x.upload.onprogress=function(e){if(e.lengthComputable)$(\"ups\").textContent=Math.round(e.loaded*100/e.total)+\"%\"};\n  x.onload=function(){var j={};try{j=JSON.parse(x.responseText)}catch(e){}\n    if(x.status===200){$(\"ups\").innerHTML=\"<span class='ok'>Enviado: \"+esc(key)+\"</span>\";ALL=null;carregarAll().then(function(){r2Abrir(R2P)})}\n    else if(x.status===409&&!sobre){   /* a lista local estava desatualizada: o servidor avisou que ja existe */\n      $(\"ups\").textContent=\"\";\n      if(confirm(\"J\u00e1 existe \"+key+\" no R2.\\nSubstituir pelo novo arquivo (\"+(f.size/1048576).toFixed(1)+\" MB)?\\nO antigo ser\u00e1 perdido.\"))r2Enviar(true);\n    }\n    else $(\"ups\").innerHTML=\"<span class='bad'>\"+esc(j.erro||(\"erro \"+x.status))+\"</span>\"};\n  x.onerror=function(){$(\"ups\").innerHTML=\"<span class='bad'>falha de rede</span>\"};\n  $(\"ups\").textContent=\"enviando...\";x.send(f);\n}\n\n\n/* ---------- envio grande em partes ---------- */\nvar MPU_PART=64*1048576,MPU_PAR=3,MPU=null;\nfunction mpuB(n){return n>=1073741824?(n/1073741824).toFixed(2)+\" GB\":(n/1048576).toFixed(1)+\" MB\"}\nfunction mpuT(s){s=Math.round(s);var h=Math.floor(s/3600),m=Math.floor(s%3600/60);return h?h+\"h\"+(\"0\"+m).slice(-2)+\"min\":m+\"min \"+(\"0\"+(s%60)).slice(-2)+\"s\"}\nfunction mpuCancelar(){if(MPU){MPU.cancel=true;MPU.xhrs.forEach(function(x){try{x.abort()}catch(e){}})}}\nfunction mpuParte(f,key,uid,n,pr){\n  return new Promise(function(ok,fail){\n    var tent=0;\n    function tenta(){\n      if(MPU.cancel||MPU.stop){fail(new Error(MPU.cancel?\"cancelado\":\"parado\"));return}\n      tent++;\n      var ini=(n-1)*MPU_PART,blob=f.slice(ini,Math.min(f.size,ini+MPU_PART));\n      var x=new XMLHttpRequest();MPU.xhrs.push(x);\n      x.open(\"PUT\",\"/jkpainel17/api/r2/mpu/part?key=\"+encodeURIComponent(key)+\"&uploadId=\"+encodeURIComponent(uid)+\"&n=\"+n);\n      x.setRequestHeader(\"Authorization\",\"Bearer \"+TK);\n      x.upload.onprogress=function(e){pr(e.loaded)};\n      x.onload=function(){\n        MPU.xhrs=MPU.xhrs.filter(function(y){return y!==x});\n        var j={};try{j=JSON.parse(x.responseText)}catch(e){}\n        if(x.status===200&&j.etag){pr(blob.size);ok(j.etag)}\n        else if(tent<6&&x.status!==403&&x.status!==400){pr(0);setTimeout(tenta,2000*tent)}\n        else fail(new Error(j.erro||(\"erro \"+x.status)))};\n      x.onerror=function(){MPU.xhrs=MPU.xhrs.filter(function(y){return y!==x});if(tent<6){pr(0);setTimeout(tenta,2000*tent)}else fail(new Error(\"falha de rede\"))};\n      x.onabort=function(){fail(new Error(\"cancelado\"))};\n      x.send(blob);\n    }\n    tenta();\n  });\n}\nasync function mpuEnviar(f,key,sobre){\n  if(MPU){alert(\"J\u00e1 existe um envio em andamento.\");return}\n  var total=f.size,np=Math.ceil(total/MPU_PART),st=$(\"ups\");\n  if(np>10000){alert(\"Arquivo grande demais para o painel.\");return}\n  var sk=\"pn_mpu:\"+key+\":\"+f.size+\":\"+f.lastModified,saved=null,uid=\"\",parts={};\n  try{saved=JSON.parse(ler(sk)||\"null\")}catch(e){}\n  MPU={cancel:false,xhrs:[]};\n  var ptxt=\"Tamanho: \"+mpuB(total)+\" em \"+np+\" partes de 64 MB.\";\n  try{\n    if(saved&&saved.uploadId&&saved.parts&&confirm(\"Existe um envio anterior incompleto deste mesmo arquivo (\"+Object.keys(saved.parts).length+\" de \"+np+\" partes j\u00e1 enviadas).\\nContinuar de onde parou?\\n\\nOK = continuar   Cancelar = recome\u00e7ar do zero\")){uid=saved.uploadId;parts=saved.parts}\n    else{\n      if(saved&&saved.uploadId){try{await api(\"r2/mpu/abort?key=\"+encodeURIComponent(key)+\"&uploadId=\"+encodeURIComponent(saved.uploadId),\"POST\")}catch(e){}}\n      var s=await api(\"r2/mpu/start?key=\"+encodeURIComponent(key)+\"&ct=\"+encodeURIComponent(f.type||\"application/octet-stream\")+sobre,\"POST\");\n      uid=s.uploadId;parts={};guardar(sk,JSON.stringify({uploadId:uid,parts:parts}));\n    }\n    var done={},fila=[],feitos=0,t0=Date.now(),base=0;\n    Object.keys(parts).forEach(function(n){done[n]=true;base+=Math.min(MPU_PART,total-(n-1)*MPU_PART)});\n    for(var n=1;n<=np;n++)if(!parts[n])fila.push(n);\n    var emv={},bytesOk=base,jaFeitas=Object.keys(parts).length;\n    function mostrar(){\n      var cur=bytesOk;for(var k in emv)cur+=emv[k];\n      var pct=Math.min(100,cur*100/total),dt=(Date.now()-t0)/1000,v=dt>3?(cur-base)/dt:0;\n      st.innerHTML=\"<div style='background:#1b2a4a;border-radius:6px;height:12px;margin:8px 0'><div style='background:#2f7d4f;height:12px;border-radius:6px;width:\"+pct.toFixed(1)+\"%'></div></div>\"+\n        pct.toFixed(1)+\"% \u00b7 \"+mpuB(cur)+\" de \"+mpuB(total)+(v?\" \u00b7 \"+mpuB(v)+\"/s \u00b7 faltam ~\"+mpuT((total-cur)/v):\"\")+\n        \"<br><span class='mut'>\"+ptxt+\" N\u00e3o feche esta aba. <a href='#' onclick='mpuCancelar();return false'>Cancelar envio</a></span>\";\n    }\n    var tick=setInterval(mostrar,500);\n    var tok=setInterval(function(){api(\"refresh\",\"POST\").then(function(j){TK=j.token;guardar(\"pn_tk\",TK)}).catch(function(){})},20*60*1000);\n    window.onbeforeunload=function(){return \"Envio em andamento\"};\n    async function worker(){\n      while(fila.length&&!MPU.cancel&&!MPU.stop){\n        var n=fila.shift();\n        var etag;try{etag=await mpuParte(f,key,uid,n,function(b){emv[n]=b})}catch(e){MPU.stop=true;throw e}\n        delete emv[n];bytesOk+=Math.min(MPU_PART,total-(n-1)*MPU_PART);parts[n]=etag;\n        guardar(sk,JSON.stringify({uploadId:uid,parts:parts}));\n      }\n    }\n    try{\n      var ws=[];for(var i=0;i<MPU_PAR;i++)ws.push(worker());\n      var rs=await Promise.allSettled(ws);\n      var ruim=rs.filter(function(r){return r.status===\"rejected\"});\n      if(ruim.length){var rc=ruim.filter(function(r){return String(r.reason&&r.reason.message)!==\"parado\"});throw (rc[0]||ruim[0]).reason}\n    }finally{clearInterval(tick);clearInterval(tok);window.onbeforeunload=null}\n    if(MPU.cancel)throw new Error(\"cancelado\");\n    st.textContent=\"Juntando as partes no R2...\";\n    var lista=Object.keys(parts).map(function(n){return{partNumber:+n,etag:parts[n]}});\n    if(lista.length!==np)throw new Error(\"faltam partes (\"+lista.length+\" de \"+np+\")\");\n    await api(\"r2/mpu/complete?key=\"+encodeURIComponent(key)+\"&uploadId=\"+encodeURIComponent(uid)+sobre,\"POST\",{parts:lista});\n    guardar(sk,\"\");MPU=null;\n    st.innerHTML=\"<span class='ok'>Enviado: \"+esc(key)+\" (\"+mpuB(total)+\")</span>\";\n    ALL=null;carregarAll().then(function(){r2Abrir(R2P)});\n  }catch(e){\n    var cancelou=MPU&&MPU.cancel;MPU=null;\n    if(cancelou){try{await api(\"r2/mpu/abort?key=\"+encodeURIComponent(key)+\"&uploadId=\"+encodeURIComponent(uid),\"POST\")}catch(e2){}guardar(sk,\"\");st.innerHTML=\"<span class='bad'>Envio cancelado.</span>\"}\n    else st.innerHTML=\"<span class='bad'>\"+esc(e.message)+\"</span><br><span class='mut'>Se foi queda de internet ou da aba, \u00e9 s\u00f3 enviar o mesmo arquivo de novo: o painel pergunta se quer continuar de onde parou.</span>\";\n  }\n}\n\n/* ---------- ABA PREVIA ---------- */\nvar PCAT=\"all\";\nfunction desenharPrev(){\n  var v=$(\"v_prev\");if(!M){v.innerHTML=\"\";return}\n  var h=\"<div class='bar'><h2 style='margin:0'>Pr\u00e9via do app</h2><span class='mut'>Mostra o cat\u00e1logo como est\u00e1 no editor agora (mesmo sem salvar). \u00c9 uma pr\u00e9via aproximada do layout.</span></div><div class='chips'>\"+\n   M.cats.map(function(c){return \"<span class='chip\"+(c[0]===PCAT?\" on\":\"\")+\"' onclick='PCAT=\\\"\"+esc(c[0])+\"\\\";desenharPrev()'>\"+esc(c[1])+\"</span>\"}).join(\"\")+\"</div><div class='grid'>\";\n  M.games.forEach(function(g,i){\n    if(PCAT!==\"all\"&&g.f[0]!==PCAT)return;\n    h+=\"<div class='pc' onclick='prevCard(\"+i+\")'><img src='\"+esc(g.f[1])+\"' onerror=\\\"this.style.visibility='hidden'\\\"><div><small>\"+esc(g.f[2])+\" \u00b7 \"+esc(g.f[3])+\"</small><b>\"+esc(g.f[4])+\"</b><small>\"+g.btns.length+\" bot\u00f5es</small></div></div>\";\n  });\n  v.innerHTML=h+\"</div>\";\n}\nfunction prevCard(i){\n  var g=M.games[i];\n  var h=\"<div style='display:flex;gap:14px'><img src='\"+esc(g.f[1])+\"' style='width:200px;height:auto;aspect-ratio:744/1254;object-fit:contain;border-radius:10px'><div><span class='pill'>\"+esc(g.f[2])+\"</span><h3 style='margin:6px 0'>\"+esc(g.f[4])+\"</h3><div class='mut'>\"+esc(g.f[3])+\"</div><p>\"+esc(g.f[5])+\"</p></div></div><div style='margin-top:12px'>\";\n  g.btns.forEach(function(b){var a=b.f[7]||\"pkg\";h+=\"<div class='cd' style='cursor:default'><div class='t'><b>\"+esc(b.f[0])+\"</b><span>\"+esc(b.f[1])+\" \u00b7 \"+esc(a)+(a===\"pkg\"&&b.f[4]!==\"0\"?\" \u00b7 \"+fs(b.f[4]):\"\")+\"</span></div></div>\"});\n  abrir(\"Card: \"+g.f[4],h+\"</div>\");\n}\n\n/* ---------- ABA SEM USO ---------- */\n/* ---------- gerador de capa (moldura PS4) ---------- */\nvar CVIMG=null;\nfunction capaAbrir(){\n  CVIMG=null;\n  var t=(M.games[SEL].f[4]||\"novo\").toLowerCase().normalize(\"NFD\").replace(/[\u0300-\u036f]/g,\"\").replace(/[^a-z0-9]+/g,\"_\").replace(/^_+|_+$/g,\"\")||\"novo\";\n  var nome=\"COVERS/capa_\"+t+\".png\",n=2;while(ALL&&ALL[nome]){nome=\"COVERS/capa_\"+t+\"_\"+n+\".png\";n++}\n  abrir(\"Gerar capa com a moldura do PS4\",\n   \"<div style='display:flex;gap:18px;align-items:flex-start;flex-wrap:wrap'><canvas id='cvc' width='744' height='1254' style='width:280px;height:472px;border-radius:8px;background:#000;flex:none'></canvas><div style='flex:1;min-width:300px'><div class='fg'>\"+\n   \"<div class='w'><label>Imagem do seu PC (capa do jogo, arte, etc.)</label><input type='file' id='cvf' accept='image/*' onchange='capaImg(this)'></div>\"+\n   \"<div><label>Tipo</label><select id='cvt' onchange='capaTipo()'><option value='jogo'>Jogo / Patch</option><option value='pasta'>Plugins &amp; Mods / Pasta</option></select></div>\"+\n   \"<div><label>Ajuste da imagem</label><select id='cvj' onchange='capaDes()'><option value='fill'>Preencher (corta as bordas)</option><option value='fit'>Mostrar inteira</option></select></div>\"+\n   \"<div><label>Texto do topo (\u00e0 direita)</label><input id='cv1' oninput='capaDes()'></div>\"+\n   \"<div><label>Etiqueta amarela</label><input id='cv2' oninput='capaDes()'></div>\"+\n   \"<div class='w'><label>Texto de baixo (\u00e0 direita)</label><input id='cv3' oninput='capaDes()'></div>\"+\n   \"<div><label>Zoom</label><input type='range' id='cvz' min='100' max='250' value='100' oninput='capaDes()'></div>\"+\n   \"<div><label>Posi\u00e7\u00e3o horizontal</label><input type='range' id='cvx' min='0' max='100' value='50' oninput='capaDes()'></div>\"+\n   \"<div><label>Posi\u00e7\u00e3o vertical</label><input type='range' id='cvy' min='0' max='100' value='50' oninput='capaDes()'></div>\"+\n   \"<div class='w'><label>Nome do arquivo no R2 (n\u00e3o substitui arquivo existente)</label><input id='cvn' value='\"+esc(nome)+\"'></div></div>\"+\n   \"<p style='margin-top:12px'><button class='p' onclick='capaEnviar()'>Enviar para o R2 e usar neste card</button> <button onclick='capaBaixar()'>Baixar PNG</button> <span id='cvs' class='mut'></span></p></div></div>\");\n  capaTipo();\n}\nfunction capaTipo(){\n  var p=$(\"cvt\").value===\"pasta\";\n  $(\"cv1\").value=p?\"PLUGINS & MODS\":\"FUTEBOL\";\n  $(\"cv2\").value=p?\"PASTA\":\"DISPON\u00cdVEL\";\n  $(\"cv3\").value=p?\"APPS \u2022 NARRADORES \u2022 PLACARES\":\"NOVO PATCH\";\n  capaDes();\n}\nfunction capaImg(inp){\n  var f=inp.files[0];if(!f)return;\n  var im=new Image();im.onload=function(){CVIMG=im;capaDes()};im.onerror=function(){alert(\"N\u00e3o consegui abrir essa imagem.\")};\n  im.src=URL.createObjectURL(f);\n}\nfunction capaDes(){\n  var c=$(\"cvc\");if(!c)return;\n  var x=c.getContext(\"2d\"),W=744,H=1254,pasta=$(\"cvt\").value===\"pasta\";\n  var go=function(){\n    x.clearRect(0,0,W,H);\n    x.fillStyle=\"#040a15\";x.fillRect(0,0,W,H);\n    // area da imagem\n    var ax=3,ay=114,aw=738,ah=1011;\n    x.fillStyle=\"#fff\";x.fillRect(0,112,W,1017);\n    x.fillStyle=\"#05070d\";x.fillRect(ax,ay,aw,ah);\n    if(CVIMG){\n      var iw=CVIMG.naturalWidth,ih=CVIMG.naturalHeight,fit=$(\"cvj\").value===\"fit\";\n      var s=fit?Math.min(aw/iw,ah/ih):Math.max(aw/iw,ah/ih)*(+$(\"cvz\").value/100);\n      var dw=iw*s,dh=ih*s;\n      var dx=ax+(aw-dw)*(+$(\"cvx\").value/100),dy=ay+(ah-dh)*(+$(\"cvy\").value/100);\n      if(fit){dx=ax+(aw-dw)/2;dy=ay+(ah-dh)/2}\n      x.save();x.beginPath();x.rect(ax,ay,aw,ah);x.clip();x.drawImage(CVIMG,dx,dy,dw,dh);x.restore();\n    }else{\n      x.fillStyle=\"#8c93a6\";x.font=\"600 28px Poppins,Arial,sans-serif\";x.textAlign=\"center\";x.textBaseline=\"middle\";x.fillText(\"escolha uma imagem\",W/2,ay+ah/2);\n    }\n    if(pasta){\n      x.strokeStyle=\"#22e6ff\";x.lineWidth=5;\n      [[30,138,1,1],[714,138,-1,1],[30,1100,1,-1],[714,1100,-1,-1]].forEach(function(q){x.beginPath();x.moveTo(q[0]+q[2]*68,q[1]);x.lineTo(q[0],q[1]);x.lineTo(q[0],q[1]+q[3]*68);x.stroke()});\n    }\n    // linha amarela\n    x.fillStyle=\"#ffcd3c\";x.fillRect(0,1126,W,3);\n    // topo\n    var g=x.createLinearGradient(0,0,0,112);g.addColorStop(0,\"#0d44ba\");g.addColorStop(1,\"#082678\");\n    x.fillStyle=g;x.fillRect(0,0,W,112);\n    x.fillStyle=\"#fff\";x.fillRect(0,112,W,2);\n    x.textBaseline=\"middle\";x.textAlign=\"left\";x.fillStyle=\"#fff\";x.font=\"800 60px Poppins,Arial,sans-serif\";x.fillText(\"PS4\",40,58);\n    x.textAlign=\"right\";x.fillStyle=\"#bed7ff\";x.font=\"700 26px Poppins,Arial,sans-serif\";\n    try{x.letterSpacing=\"2px\"}catch(e){}\n    x.fillText(($(\"cv1\").value||\"\").toUpperCase(),706,56);\n    try{x.letterSpacing=\"0px\"}catch(e){}\n    // rodape\n    var tag=($(\"cv2\").value||\"\").toUpperCase();\n    x.font=\"800 34px Poppins,Arial,sans-serif\";\n    var tw=x.measureText(tag).width,pw=Math.max(150,tw+64),py=1158,ph=64;\n    x.fillStyle=\"#ffcd3c\";x.beginPath();\n    if(x.roundRect)x.roundRect(38,py,pw,ph,32);else x.rect(38,py,pw,ph);\n    x.fill();\n    x.fillStyle=\"#17361f\";x.textAlign=\"center\";x.fillText(tag,38+pw/2,py+ph/2+2);\n    x.textAlign=\"right\";x.fillStyle=\"#bed7ff\";x.font=\"600 26px Poppins,Arial,sans-serif\";\n    x.fillText(($(\"cv3\").value||\"\").toUpperCase(),706,py+ph/2+2);\n  };\n  if(document.fonts&&document.fonts.load){Promise.all([document.fonts.load(\"800 34px Poppins\"),document.fonts.load(\"700 26px Poppins\"),document.fonts.load(\"600 26px Poppins\")]).then(go,go)}else go();\n}\nfunction capaBlob(cb){$(\"cvc\").toBlob(cb,\"image/png\")}\nfunction capaBaixar(){\n  if(!CVIMG){alert(\"Escolha uma imagem primeiro.\");return}\n  capaBlob(function(b){var a=document.createElement(\"a\");a.href=URL.createObjectURL(b);a.download=($(\"cvn\").value.split(\"/\").pop()||\"capa.png\");document.body.appendChild(a);a.click();a.remove()});\n}\nfunction capaEnviar(){\n  if(!CVIMG){alert(\"Escolha uma imagem primeiro.\");return}\n  var key=$(\"cvn\").value.trim().replace(/^\\/+/,\"\");\n  if(!/\\.png$/i.test(key)){alert(\"O nome precisa terminar em .png\");return}\n  if(ALL&&ALL[key]){alert(\"J\u00e1 existe \"+key+\". O painel n\u00e3o substitui arquivos: mude o nome.\");return}\n  capaBlob(function(b){\n    var x=new XMLHttpRequest();x.open(\"PUT\",\"/jkpainel17/api/r2/upload?key=\"+encodeURIComponent(key));\n    x.setRequestHeader(\"Authorization\",\"Bearer \"+TK);x.setRequestHeader(\"Content-Type\",\"image/png\");\n    x.onload=function(){var j={};try{j=JSON.parse(x.responseText)}catch(e){}\n      if(x.status===200){\n        if(ALL){ALL[key]={k:key,s:b.size,u:new Date().toISOString()};ALL._n=(ALL._n||0)+1}\n        M.games[SEL].f[1]=urlOf(key);sujo();fechar();desenharCat();alert(\"Capa enviada e aplicada no card. Falta clicar em Salvar cat\u00e1logo.\");\n      }else $(\"cvs\").innerHTML=\"<span class='bad'>\"+esc(j.erro||(\"erro \"+x.status))+\"</span>\"};\n    x.onerror=function(){$(\"cvs\").innerHTML=\"<span class='bad'>falha de rede</span>\"};\n    $(\"cvs\").textContent=\"enviando...\";x.send(b);\n  });\n}\n\n/* ---------- gerador de video (.jkv) ---------- */\nvar ADS=[7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,27086,29794,32767];\nvar ADI=[-1,-1,-1,-1,2,4,6,8];\nfunction adpcmBlock(s,o,n){\n  var pred=s[o],idx=0,nib=[],out=new Uint8Array(4+((n-1+1)>>1));\n  for(var i=1;i<n;i++){\n    var step=ADS[idx],diff=s[o+i]-pred,code=0;\n    if(diff<0){code=8;diff=-diff}\n    var d=step>>3;\n    if(diff>=step){code|=4;diff-=step;d+=step}\n    var s2=step>>1;if(diff>=s2){code|=2;diff-=s2;d+=s2}\n    var s4=step>>2;if(diff>=s4){code|=1;d+=s4}\n    pred=(code&8)?pred-d:pred+d;if(pred>32767)pred=32767;if(pred<-32768)pred=-32768;\n    idx+=ADI[code&7];if(idx<0)idx=0;if(idx>88)idx=88;\n    nib.push(code);\n  }\n  if(nib.length%2)nib.push(0);\n  out[2]=0;out[3]=0;\n  // o cabecalho guarda o predictor INICIAL (amostra 0), igual ao script em Python\n  out[0]=s[o]&255;out[1]=(s[o]>>8)&255;\n  for(var k=0;k<nib.length;k+=2)out[4+(k>>1)]=nib[k]|(nib[k+1]<<4);\n  return out.subarray(0,4+(nib.length>>1));\n}\nvar JKT=null,JKB=null,JKCANCEL=false;\nfunction jkvAbrir(i){\n  JKT=i<0?{t:\"card\"}:{t:\"btn\",i:i};JKB=null;JKCANCEL=false;\n  var g=M.games[SEL],t=(g.f[4]||\"video\").toLowerCase().normalize(\"NFD\").replace(/[\\u0300-\\u036f]/g,\"\").replace(/[^a-z0-9]+/g,\"_\").replace(/^_+|_+$/g,\"\")||\"video\";\n  var nome=\"JKV/\"+t+\".jkv\",n=2;while(ALL&&ALL[nome]){nome=\"JKV/\"+t+\"_\"+n+\".jkv\";n++}\n  abrir(\"Gerar v\u00eddeo (.jkv) a partir de um MP4\",\n   \"<p class='mut'>O v\u00eddeo \u00e9 convertido aqui no seu navegador (o MP4 n\u00e3o \u00e9 enviado). Deixe esta aba aberta at\u00e9 terminar. Quanto maior a largura/fps/dura\u00e7\u00e3o, maior o arquivo, e o app baixa o arquivo inteiro antes de tocar.</p>\"+\n   \"<div class='fg'><div class='w'><label>V\u00eddeo (MP4, MOV, WEBM)</label><input type='file' id='jkf' accept='video/*'></div>\"+\n   \"<div><label>Largura (px)</label><input id='jkw' value='480'></div><div><label>Quadros por segundo</label><input id='jkr' value='15'></div>\"+\n   \"<div><label>Qualidade JPEG (1-100)</label><input id='jkq' value='70'></div><div><label>Dura\u00e7\u00e3o m\u00e1xima (segundos)</label><input id='jkm' value='120'></div>\"+\n   \"<div class='w'><label>Nome do arquivo no R2 (se j\u00e1 existir, pergunta se quer substituir)</label><input id='jkn' value='\"+esc(nome)+\"'></div></div>\"+\n   \"<p style='margin-top:12px'><button class='p' id='jkgo' onclick='jkvConverter()'>Converter</button> <button id='jkup' onclick='jkvEnviar()' disabled>Enviar para o R2 e usar</button> <button id='jkdl' onclick='jkvBaixar()' disabled>Baixar .jkv</button> <button onclick='JKCANCEL=true'>Cancelar</button></p>\"+\n   \"<div style='background:#16181f;border-radius:8px;height:10px;overflow:hidden'><div id='jkbar' style='height:10px;width:0;background:#35d07f'></div></div><p id='jks' class='mut'></p>\");\n}\nfunction jkSt(t,c){var e=$(\"jks\");if(e){e.className=c||\"mut\";e.textContent=t}}\nfunction jkvSeek(v,t){return new Promise(function(res,rej){\n  var done=false,to=setTimeout(function(){if(!done){done=true;v.removeEventListener(\"seeked\",f);rej(new Error(\"o v\u00eddeo travou ao buscar um quadro\"))}},15000);\n  function f(){if(done)return;done=true;clearTimeout(to);v.removeEventListener(\"seeked\",f);res()}\n  v.addEventListener(\"seeked\",f);v.currentTime=t;\n})}\nasync function jkvConverter(){\n  var f=$(\"jkf\").files[0];if(!f){alert(\"Escolha um v\u00eddeo.\");return}\n  var W=Math.max(120,Math.min(960,parseInt($(\"jkw\").value,10)||480));W-=W%2;\n  var FPS=Math.max(5,Math.min(30,parseInt($(\"jkr\").value,10)||15));\n  var Q=Math.max(1,Math.min(100,parseInt($(\"jkq\").value,10)||70))/100;\n  var MAXS=Math.max(1,Math.min(600,parseFloat($(\"jkm\").value)||120));\n  JKCANCEL=false;JKB=null;$(\"jkgo\").disabled=true;$(\"jkup\").disabled=true;$(\"jkdl\").disabled=true;$(\"jkbar\").style.width=\"0\";\n  var url=URL.createObjectURL(f);\n  try{\n    jkSt(\"Lendo o v\u00eddeo...\");\n    var v=document.createElement(\"video\");v.muted=true;v.preload=\"auto\";v.playsInline=true;v.src=url;\n    await new Promise(function(res,rej){v.onloadeddata=res;v.onerror=function(){rej(new Error(\"o navegador n\u00e3o conseguiu abrir esse v\u00eddeo\"))}});\n    var dur=Math.min(v.duration||0,MAXS);if(!isFinite(dur)||dur<=0)throw new Error(\"dura\u00e7\u00e3o inv\u00e1lida\");\n    var N=Math.floor(dur*FPS),H=Math.round(W*v.videoHeight/v.videoWidth/2)*2,SPF=Math.floor(24000/FPS);\n    // audio\n    jkSt(\"Extraindo o \u00e1udio...\");\n    var pcm=new Int16Array(N*SPF),temAudio=true;\n    try{\n      var AC=window.AudioContext||window.webkitAudioContext,ac=new AC();\n      var buf=await ac.decodeAudioData((await f.arrayBuffer()));ac.close&&ac.close();\n      var oc=new OfflineAudioContext(1,Math.ceil(dur*24000)+24000,24000),src=oc.createBufferSource();src.buffer=buf;src.connect(oc.destination);src.start(0);\n      var r=await oc.startRendering(),ch=r.getChannelData(0);\n      for(var i=0;i<pcm.length&&i<ch.length;i++){var x=ch[i]*32767;pcm[i]=x>32767?32767:(x<-32768?-32768:x)}\n    }catch(e){temAudio=false}\n    var cv=document.createElement(\"canvas\");cv.width=W;cv.height=H;var cx=cv.getContext(\"2d\");\n    var parts=[],head=new Uint8Array(20),dv=new DataView(head.buffer);\n    head.set([74,75,86,49],0);dv.setUint16(4,W,true);dv.setUint16(6,H,true);dv.setUint16(8,FPS,true);dv.setUint16(10,24000,true);dv.setUint32(12,N,true);dv.setUint32(16,SPF,true);\n    parts.push(head);var total=20;\n    for(var k=0;k<N;k++){\n      if(JKCANCEL)throw new Error(\"cancelado\");\n      await jkvSeek(v,Math.min(k/FPS+0.001,v.duration-0.01));\n      cx.drawImage(v,0,0,W,H);\n      var jb=await new Promise(function(res){cv.toBlob(res,\"image/jpeg\",Q)});\n      var jpg=new Uint8Array(await jb.arrayBuffer()),ad=adpcmBlock(pcm,k*SPF,SPF);\n      var h2=new Uint8Array(8),d2=new DataView(h2.buffer);d2.setUint32(0,jpg.length,true);d2.setUint32(4,ad.length,true);\n      parts.push(h2,jpg,ad);total+=8+jpg.length+ad.length;\n      if(k%5===0||k===N-1){$(\"jkbar\").style.width=Math.round((k+1)*100/N)+\"%\";jkSt(\"Quadro \"+(k+1)+\" de \"+N+\" \u00b7 \"+fs(total))}\n    }\n    JKB=new Blob(parts,{type:\"application/octet-stream\"});\n    jkSt(\"Pronto: \"+W+\"x\"+H+\" \u00b7 \"+N+\" quadros a \"+FPS+\" fps \u00b7 \"+dur.toFixed(1)+\" s \u00b7 \"+fs(JKB.size)+(temAudio?\"\":\" \u00b7 SEM \u00c1UDIO (o navegador n\u00e3o leu o \u00e1udio desse v\u00eddeo)\")+(JKB.size>30*1048576?\" \u00b7 grande: reduza largura/fps/dura\u00e7\u00e3o\":\"\"),temAudio&&JKB.size<=30*1048576?\"ok\":\"warn\");\n    $(\"jkup\").disabled=false;$(\"jkdl\").disabled=false;\n  }catch(e){jkSt(\"Erro: \"+e.message,\"bad\")}\n  URL.revokeObjectURL(url);$(\"jkgo\").disabled=false;\n}\nfunction jkvBaixar(){if(!JKB)return;var a=document.createElement(\"a\");a.href=URL.createObjectURL(JKB);a.download=($(\"jkn\").value.split(\"/\").pop()||\"video.jkv\");document.body.appendChild(a);a.click();a.remove()}\nfunction jkvEnviar(){\n  if(!JKB)return;\n  var key=$(\"jkn\").value.trim().replace(/^\\/+/,\"\");\n  if(!/\\.jkv$/i.test(key)){alert(\"O nome precisa terminar em .jkv\");return}\n  if(JKB.size>90*1048576){alert(\"Arquivo maior que 90 MB. Reduza largura, fps ou dura\u00e7\u00e3o.\");return}\n  var sobre=false;\n  if(ALL&&ALL[key]){if(!confirm(\"J\u00e1 existe \"+key+\" (\"+fs(ALL[key].s)+\").\\n\\nSubstituir pelo v\u00eddeo novo? O v\u00eddeo antigo ser\u00e1 perdido.\"))return;sobre=true}\n  var x=new XMLHttpRequest();x.open(\"PUT\",\"/jkpainel17/api/r2/upload?key=\"+encodeURIComponent(key)+(sobre?\"&overwrite=1\":\"\"));\n  x.setRequestHeader(\"Authorization\",\"Bearer \"+TK);x.setRequestHeader(\"Content-Type\",\"application/octet-stream\");\n  x.upload.onprogress=function(e){if(e.lengthComputable)jkSt(\"Enviando \"+Math.round(e.loaded*100/e.total)+\"%\")};\n  x.onload=function(){var j={};try{j=JSON.parse(x.responseText)}catch(e){}\n    if(x.status===200){\n      if(ALL){ALL[key]={k:key,s:JKB.size,u:new Date().toISOString()};ALL._n=(ALL._n||0)+1}\n      var g=M.games[SEL],uu=urlOf(key)+(sobre?\"?v=\"+Date.now():\"\");if(JKT.t===\"card\")g.f[6]=uu;else g.btns[JKT.i].f[2]=uu;\n      sujo();fechar();desenharCat();alert(\"V\u00eddeo enviado e aplicado. Falta clicar em Salvar cat\u00e1logo.\");\n    }else jkSt(\"Erro: \"+(j.erro||(\"erro \"+x.status)),\"bad\")};\n  x.onerror=function(){jkSt(\"Erro: falha de rede\",\"bad\")};\n  jkSt(\"Enviando...\");x.send(JKB);\n}\n\n/* ---------- linha de atualizacao do app (APP versao url content_id tamanho tipo) ---------- */\nfunction appIdx(){for(var i=0;i<M.tail.length;i++)if(M.tail[i].indexOf(\"APP\\t\")===0)return i;return -1}\nfunction appAbrir(){\n  var i=appIdx(),f=i>=0?M.tail[i].split(\"\\t\"):[\"APP\",\"\",\"\",\"IV0000-PSTR00002_00-PS4PATCHSTORE000\",\"0\",\"PS4GD\"];\n  while(f.length<6)f.push(\"\");\n  abrir(\"Atualiza\u00e7\u00e3o do app (vers\u00e3o nova do JK Patchs)\",\n   \"<p class='mut'>Quando voc\u00ea lan\u00e7ar um app novo: suba o PKG no R2 (aba Arquivos), preencha aqui a vers\u00e3o e o link, e salve o cat\u00e1logo. Os apps com vers\u00e3o menor passam a baixar a atualiza\u00e7\u00e3o sozinhos. \"+(i<0?\"<b class='warn'>O cat\u00e1logo n\u00e3o tem essa linha hoje: ela ser\u00e1 criada.</b>\":\"\")+\"</p>\"+\n   \"<div class='fg'><div><label>Vers\u00e3o (formato N.NN, ex.: 1.19)</label><input id='ap1' value='\"+esc(f[1])+\"'></div>\"+\n   \"<div><label>Tipo</label><input id='ap5' value='\"+esc(f[5]||\"PS4GD\")+\"'></div>\"+\n   \"<div class='w'><label>Link do PKG no R2 <button class='s' onclick='appEscolher()'>escolher do R2</button></label><input id='ap2' value='\"+esc(f[2])+\"'></div>\"+\n   \"<div><label>content_id</label><input id='ap3' value='\"+esc(f[3])+\"'></div>\"+\n   \"<div><label>Tamanho (bytes) <button class='s' onclick='appTam()'>pegar do R2</button></label><input id='ap4' value='\"+esc(f[4])+\"'></div></div>\"+\n   \"<p style='margin-top:12px'><button class='p' onclick='appAplicar()'>Aplicar no cat\u00e1logo</button> <span class='mut'>depois clique em Salvar cat\u00e1logo</span></p><p id='aps' class='bad'></p>\");\n}\nvar APPV=null;\nfunction appEscolher(){APPV={a:$(\"ap1\").value,b:$(\"ap5\").value,c:$(\"ap3\").value,d:$(\"ap4\").value};PK={t:\"app\"};abrir(\"Escolher o PKG do app\",\"\");pkAbrir(\"APP-PS4/\")}\nfunction appTam(){var k=keyOf($(\"ap2\").value);if(k&&ALL&&ALL[k]){$(\"ap4\").value=String(ALL[k].s)}else $(\"aps\").textContent=\"Esse link n\u00e3o est\u00e1 no R2 (ou a lista do R2 n\u00e3o carregou).\"}\nfunction appAplicar(){\n  var v=$(\"ap1\").value.trim(),u=$(\"ap2\").value.trim(),c=$(\"ap3\").value.trim(),z=$(\"ap4\").value.trim(),t=$(\"ap5\").value.trim()||\"PS4GD\",e=$(\"aps\");\n  if(!/^\\d+\\.\\d\\d$/.test(v)){e.textContent=\"Vers\u00e3o deve ser no formato N.NN (ex.: 1.19).\";return}\n  if(!/^https:\\/\\//.test(u)){e.textContent=\"O link do PKG precisa come\u00e7ar com https://\";return}\n  if(!/^[A-Z0-9_-]{20,40}$/i.test(c)){e.textContent=\"content_id inv\u00e1lido.\";return}\n  if(!/^\\d+$/.test(z)||+z<1000000){e.textContent=\"Tamanho inv\u00e1lido (use o bot\u00e3o pegar do R2).\";return}\n  var k=keyOf(u);\n  if(ALL&&k&&!ALL[k]){e.textContent=\"Esse PKG ainda n\u00e3o existe no R2. Suba o arquivo antes.\";return}\n  if(ALL&&k&&ALL[k]&&String(ALL[k].s)!==z){e.textContent=\"O tamanho informado \u00e9 diferente do arquivo no R2 (\"+ALL[k].s+\"). Use o bot\u00e3o pegar do R2.\";return}\n  var ln=[\"APP\",v,u,c,z,t].join(\"\\t\"),i=appIdx();\n  if(i>=0)M.tail[i]=ln;else M.tail.push(ln);\n  sujo();fechar();alert(\"Linha do app atualizada (vers\u00e3o \"+v+\"). Falta clicar em Salvar cat\u00e1logo.\");\n}\n\n/* ---------- inicio ---------- */\n(function(){var t=ler(\"pn_tk\");if(t){TK=t;api(\"me\").then(function(){iniciar()},function(){mostrarLogin(\"\")})}else mostrarLogin(\"\")})();\n</script>\n</body></html>\n";

// ---------- WORKER ----------
export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const p = url.pathname;

    try {
      // Download publico funciona sem exigir login ou configuracao do KV.
      if (p === "/download-app") {
        return await downloadApp(request, env);
      }
      const tv = p.match(/^\/(t[123])\/JKPATCHS\.pkg$/) || p.match(/^\/(t4)\/TESTE\.pkg$/);
      if (tv) {
        return await downloadApp(request, env, tv[1]);
      }
      if (p === "/testar") {
        return new Response(
          '<html><body style="background:#111;color:#fff;font-size:28px;font-family:Arial"><p>Teste de download (toque em cada um):</p>' +
          '<p><a style="color:#6cf" href="/t1/JKPATCHS.pkg">TESTE 1 - sem attachment</a></p>' +
          '<p><a style="color:#6cf" href="/t2/JKPATCHS.pkg">TESTE 2 - com attachment</a></p>' +
          '<p><a style="color:#6cf" href="/t3/JKPATCHS.pkg">TESTE 3 - tipo x-ps4-pkg</a></p><p><a style="color:#6cf" href="/t4/TESTE.pkg">TESTE 4 - outro PKG (APP-PS4/TESTE.pkg)</a></p></body></html>',
          { headers: { "Content-Type": "text/html; charset=utf-8" } }
        );
      }

      if (p === "/instalar") {
        return new Response(INSTALAR_HTML, { headers: { "Content-Type": "text/html; charset=utf-8", "Cache-Control": "no-store" } });
      }
      if (p === "/q.bin") {
        const obj = await env.APP_BUCKET.head("APP-PS4/JKPATCHS.pkg");
        if (!obj) return text(404, "ERRO\nInstalador nao encontrado");
        const u = jkPayload("http://" + url.host + "/t1/JKPATCHS.pkg", "JK PATCHS", "IV0000-PSTR00002_00-PS4PATCHSTORE000", "PS4GD", obj.size);
        if (!u) return text(500, "ERRO\npayload invalido");
        return new Response(u, { headers: { "Content-Type": "application/octet-stream", "Cache-Control": "no-store" } });
      }
      if (p === "/p.bin") {
        const u = dpiPayload(url.searchParams.get("ip"), url.searchParams.get("port"));
        if (!u) return text(400, "ERRO\nip/porta invalidos");
        return new Response(u, { headers: { "Content-Type": "application/octet-stream", "Cache-Control": "no-store" } });
      }

      if (p === "/video-app") {
        return await videoApp(request, env);
      }

      // Pagina publica com o botao de download e o video.
      if (p === "/baixar" || p === "/baixar/") {
        return landingPage();
      }

      // Demais rotas precisam das configuracoes de autenticacao.
      if (!env.DB || !env.TOKEN_SECRET || !env.ADMIN_KEY) {
        return text(
          500,
          "ERRO\nservidor sem configuracao (DB, TOKEN_SECRET, ADMIN_KEY)"
        );
      }

      if (p === "/login" && request.method === "POST") {
        return await login(request, env);
      }

      if (p === "/register" && request.method === "POST") {
        return await register(request, env);
      }

      if (p === "/me" && request.method === "GET") {
        return await me(request, env, url);
      }

      if (p === "/plans" && request.method === "GET") {
        return plans();
      }

      if (p === "/catalog" && request.method === "GET") {
        return await catalog(request, env, url);
      }

      if (p === "/preview" && request.method === "GET") {
        return await preview(env);
      }

      if (p === "/jkpainel17" || p === "/jkpainel17/") {
        return new Response(PANEL_HTML, { headers: { "Content-Type": "text/html; charset=utf-8", "Cache-Control": "no-store" } });
      }

      if (p === "/jkpainel17/logo.png") {
        return panelLogoResponse();
      }

      if (p.startsWith("/jkpainel17/api/")) {
        return await panelApi(request, env, url);
      }

      if (p === "/jkpainel17/usuarios") {
        return new Response(ADMIN_HTML, {
          headers: {
            "Content-Type": "text/html; charset=utf-8",
            "Cache-Control": "no-store"
          }
        });
      }

      if (p.startsWith("/admin/api/")) {
        return await admin(request, env, url);
      }

      if (p === "/") {
        return text(200, "PS4 Patch Store");
      }

      return text(404, "nao encontrado");

    } catch (e) {
      return text(500, "ERRO\ninterno");
    }
  }
};