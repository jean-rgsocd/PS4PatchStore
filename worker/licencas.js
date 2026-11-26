// JK PATCHS - CENTRAL DE LICENCAS (D2 produtos/dependencias, D3 licencas/consoles, F painel central, auditoria)
//
// Tudo no KV "DB" (sem D1). Chaves NOVAS (as antigas nao sao alteradas):
//   prod:all        {"v":1,"itens":{id:produto}}           catalogo administrativo de PRX/CPK/PKG
//   planos          {"v":1,"itens":{id:plano}}             planos editaveis (sem isso valem os PLANOS do worker.js)
//   lic:<user>      {"v":1,"itens":[licenca]}              licencas explicitas do cliente (permanente, teste, ...)
//   con:<cid>       console (metadados: user, status, ultimo)
//   conu:<user>     ["cid", ...]                            consoles vinculados a conta
//   licfg           {"dias":7,"tolerancia":0}               validade das autorizacoes assinadas
//   padm:<user>     equipe do painel (salt, hash, papel)
//   aud:<ts>:<rnd>  auditoria administrativa (metadados, 1 ano)
//   sec:<ts>:<rnd>  eventos de seguranca: acesso negado, console de outra conta (90 dias)
//
// Secret NOVO (opcional ate a fase H): LIC_PRIV = chave privada Ed25519 (PKCS8 em base64).
// Sem ele, /lic/sync responde "licencas nao configuradas" e nada mais muda.
//
// Autorizacao assinada (arquivo /data/jk/lic/<modulo>.lic no console):
//   JKL1|<modulo>|<cid>|<uh>|<versao minima>|<emitida>|<vence>|<assinatura Ed25519 em hex>
//   cid = 24 hex de SHA-512("jkcid:" + id do console enviado pelo app) - a PRX recalcula sozinha
//   A PRX confere com a chave PUBLICA embutida. Chave privada so no Cloudflare.

const enc = new TextEncoder();
const DIA = 86400;
const CACHE_MS = 60 * 1000;
const AUD_TTL = 365 * DIA, SEC_TTL = 90 * DIA;

// ---------- util ----------
export function json(status, o) {
  return new Response(JSON.stringify(o), { status, headers: { "Content-Type": "application/json; charset=utf-8", "Cache-Control": "no-store" } });
}
function texto(status, s) {
  return new Response(s, { status, headers: { "Content-Type": "text/plain; charset=utf-8", "Cache-Control": "no-store" } });
}
const hex = (buf) => [...new Uint8Array(buf)].map((b) => b.toString(16).padStart(2, "0")).join("");
const agora = () => Math.floor(Date.now() / 1000);
const rnd = () => Math.random().toString(36).slice(2, 7);
const rev = () => String(9999999999999 - Date.now()).padStart(13, "0");
const limpaUser = (u) => String(u || "").trim().toLowerCase().slice(0, 32);
const okUser = (u) => /^[a-z0-9_.-]{2,32}$/.test(u);
const okId = (s) => /^[a-z0-9][a-z0-9_.-]{0,62}$/.test(s);
async function lerJson(request) { try { return await request.json(); } catch (e) { return null; } }
async function kvJson(env, k, padrao) { try { const v = await env.DB.get(k, "json"); return v == null ? padrao : v; } catch (e) { return padrao; } }

export async function cidDe(dev) {
  return hex(await crypto.subtle.digest("SHA-512", enc.encode("jkcid:" + dev))).slice(0, 24);
}

// ---------- auditoria e seguranca ----------
export async function auditarAdmin(env, quem, acao, alvo, det) {
  const ev = { t: new Date().toISOString(), quem: String(quem || "painel").slice(0, 40), acao: String(acao).slice(0, 80),
               alvo: String(alvo || "").slice(0, 120), det: String(det || "").slice(0, 300) };
  console.log(JSON.stringify(Object.assign({ evento: "admin" }, ev)));
  try { await env.DB.put("aud:" + rev() + ":" + rnd(), "1", { metadata: ev, expirationTtl: AUD_TTL }); } catch (e) {}
}
const secRecentes = new Map();
export async function eventoSeguranca(env, tipo, user, request, motivo) {
  const chave = tipo + "|" + user + "|" + motivo;
  const t0 = secRecentes.get(chave);
  if (t0 && Date.now() - t0 < 10 * 60 * 1000) return;     // nao repete o mesmo evento por 10 min (economiza gravacoes)
  secRecentes.set(chave, Date.now());
  const ev = { t: new Date().toISOString(), tipo, user: user || "", ip: (request && request.headers.get("CF-Connecting-IP")) || "",
               pais: (request && request.cf && request.cf.country) || "", motivo: String(motivo || "").slice(0, 200) };
  console.log(JSON.stringify(Object.assign({ evento: "seguranca" }, ev)));
  try { await env.DB.put("sec:" + rev() + ":" + rnd(), "1", { metadata: ev, expirationTtl: SEC_TTL }); } catch (e) {}
}

// ---------- caches por instancia ----------
const memo = new Map();
async function cache(chave, fn) {
  const c = memo.get(chave);
  if (c && Date.now() - c.em < CACHE_MS) return c.v;
  const v = await fn();
  memo.set(chave, { v, em: Date.now() });
  return v;
}
const limpaCache = (prefixo) => { for (const k of [...memo.keys()]) if (k.startsWith(prefixo)) memo.delete(k); };

// ---------- planos ----------
// Sem "planos" no KV valem os PLANOS do worker.js (mensal/semestral/anual) com: 1 console, todos os produtos.
export async function lerPlanos(env, PLANOS_CODIGO) {
  return cache("planos", async () => {
    const doc = await kvJson(env, "planos", null);
    if (doc && doc.itens) return doc.itens;
    const out = {};
    for (const [id, p] of Object.entries(PLANOS_CODIGO || {})) {
      out[id] = { id, nome: p.nome, preco: p.preco, dias: p.dias, pix: p.pix, consoles: 1, produtos: ["*"], status: "ativo", renovavel: true };
    }
    return out;
  });
}

// ---------- produtos ----------
export async function lerProdutos(env) {
  return cache("prod", async () => (await kvJson(env, "prod:all", { v: 1, itens: {} })).itens || {});
}
async function gravarProdutos(env, itens) {
  await env.DB.put("prod:all", JSON.stringify({ v: 1, itens }));
  limpaCache("prod");
}
async function indicePorArquivo(env) {
  return cache("prodidx", async () => {
    const idx = {};
    for (const p of Object.values(await lerProdutos(env))) if (p.arquivo) idx[p.arquivo] = p;
    return idx;
  });
}
const slug = (s) => String(s).toLowerCase().normalize("NFD").replace(/[̀-ͯ]/g, "").replace(/\.[a-z0-9]+$/, "")
  .replace(/[^a-z0-9]+/g, "-").replace(/^-|-$/g, "").slice(0, 48) || "item";

// Importa do catalogo do app: cada PRX/CPK/PKG vira produto; cada card JKX vira dependencia (CPK -> PRX do card).
// So ACRESCENTA: produtos ja cadastrados (mesmo arquivo) nao sao alterados.
export function importarDoCatalogo(catalogo, itens, R2_PUBLICO) {
  const porArquivo = {};
  for (const p of Object.values(itens)) if (p.arquivo) porArquivo[p.arquivo] = p;
  const novos = [];
  const add = (url, origem) => {
    if (!url.startsWith(R2_PUBLICO)) return null;
    let chave;
    try { chave = decodeURIComponent(url.slice(R2_PUBLICO.length).split("?")[0]); } catch (e) { return null; }
    const nome = chave.slice(chave.lastIndexOf("/") + 1);
    const ext = (nome.match(/\.([a-z0-9]+)$/i) || [])[1];
    const tipo = { prx: "PRX", cpk: "CPK", pkg: "PKG" }[(ext || "").toLowerCase()];
    if (!tipo) return null;
    let p = porArquivo[chave];
    if (!p) {
      let id = tipo.toLowerCase() + "-" + slug(nome);
      for (let n = 2; itens[id]; n++) id = tipo.toLowerCase() + "-" + slug(nome) + "-" + n;
      const once = (url.match(/[?&]once=(\d+)/) || [])[1];
      p = { id, tipo, nome, arquivo: chave, versao: "", versao_min: "", sha256: "", tamanho: once ? Number(once) : 0,
            status: "ativo", deps: [], planos: ["*"], licenciado: false, modulo: "", desc: "", origem: [], criado: new Date().toISOString() };
      itens[id] = p; porArquivo[chave] = p; novos.push(id);
    }
    if (origem && !(p.origem || []).includes(origem) && (p.origem || []).length < 20) (p.origem = p.origem || []).push(origem);
    return p;
  };
  const rx = new RegExp(R2_PUBLICO.replace(/[.*+?^${}()|[\]\\/]/g, "\\$&") + "[^\\t|\\r\\n]*", "g");
  let deps = 0;
  for (const linha of String(catalogo).split(/\r?\n/)) {
    const f = linha.split("\t");
    if (f[0] === "APP" || f[0] === "MOD") continue;
    const origem = f[0] === "JKX" ? "card:" + f[1] : f[0] === "BTN" ? "botao:" + (f[2] || f[1]) : f[0] === "PLO" ? "placar:" + f[2] : f[0];
    const ps = (linha.match(rx) || []).map((u) => add(u, origem)).filter(Boolean);
    if (f[0] === "JKX" && f.length >= 8) {                   // dependencia observada: CPKs do card dependem das PRX do card
      const prxs = ps.filter((p) => p.tipo === "PRX"), cpks = ps.filter((p) => p.tipo === "CPK");
      for (const c of cpks) for (const x of prxs) {
        if (!c.deps.some((d) => d.id === x.id)) { c.deps.push({ id: x.id, min: "" }); deps++; }
      }
    }
  }
  return { novos, deps };
}

// ---------- licencas ----------
export async function lerLicencas(env, user) {
  return cache("lic:" + user, async () => (await kvJson(env, "lic:" + user, { v: 1, itens: [] })).itens || []);
}
const TIPOS_LIC = ["assinatura", "permanente", "temporaria", "teste", "promocional"];
const STATUS_LIC = ["ativa", "suspensa", "revogada"];
function cmpVersao(a, b) {
  const pa = String(a || "0").split(/[.-]/).map((x) => parseInt(x, 10) || 0), pb = String(b || "0").split(/[.-]/).map((x) => parseInt(x, 10) || 0);
  for (let i = 0; i < Math.max(pa.length, pb.length); i++) { const d = (pa[i] || 0) - (pb[i] || 0); if (d) return d < 0 ? -1 : 1; }
  return 0;
}

// Decide se o cliente pode usar o produto. Devolve {ok, motivo, ate (segundos)}.
//   1 conta ativa / suspensa   2 licenca explicita revogada/suspensa   3 licenca explicita ativa
//   4 assinatura valida e produto no plano   5 produto revogado   6 versao minima
export function autorizar({ rec, user, estado, prod, planos, licencas, versao }) {
  const t = agora();
  if (!rec) return { ok: false, motivo: "conta inexistente" };
  if (estado === "desativado") return { ok: false, motivo: "conta suspensa" };
  if (!prod) return { ok: false, motivo: "produto nao cadastrado" };
  if (prod.status !== "ativo") return { ok: false, motivo: "produto " + (prod.status || "inativo") };
  if (versao && prod.versao_min && cmpVersao(versao, prod.versao_min) < 0) return { ok: false, motivo: "versao incompativel (minima " + prod.versao_min + ")" };
  const minhas = (licencas || []).filter((l) => l.produto === prod.id || l.produto === "*");
  if (minhas.some((l) => l.status === "revogada")) return { ok: false, motivo: "licenca revogada" };
  if (minhas.some((l) => l.status === "suspensa")) return { ok: false, motivo: "licenca suspensa" };
  for (const l of minhas) {
    if (l.status !== "ativa") continue;
    if (l.tipo === "permanente") return { ok: true, ate: 0, via: "permanente" };
    const fim = Date.parse(l.fim || "") / 1000;
    if (fim && fim > t) return { ok: true, ate: fim, via: l.tipo };
  }
  if (estado !== "ativo") return { ok: false, motivo: estado === "vencido" ? "assinatura vencida" : "conta " + estado };
  const plano = planos[rec.plan || ""];
  const lista = plano ? plano.produtos || ["*"] : ["*"];      // conta antiga/sem plano cadastrado: todos (igual a hoje)
  if (plano && plano.status === "inativo") return { ok: false, motivo: "plano inativo" };
  if (!lista.includes("*") && !lista.includes(prod.id)) return { ok: false, motivo: "produto fora do plano" };
  if ((prod.planos || ["*"]).length && !(prod.planos || ["*"]).includes("*") && !(prod.planos || []).includes(rec.plan || "")) {
    return { ok: false, motivo: "produto nao liberado para o plano" };
  }
  const fimAss = rec.v2 && rec.expires ? Math.floor(Date.parse(rec.expires) / 1000) : 0;
  return { ok: true, ate: fimAss, via: "assinatura" };
}

// Usado pelo /f/: produto cadastrado e bloqueado, ou CPK que depende de PRX licenciada sem licenca = negado.
// Arquivo que nao esta cadastrado como produto: segue a regra da D1 (conta ativa).
export async function checarDownload(env, chaveR2, user, rec, estado, PLANOS_CODIGO) {
  const idx = await indicePorArquivo(env);
  const prod = idx[chaveR2];
  if (!prod) return null;
  if (prod.status !== "ativo") return "produto indisponivel";
  const itens = await lerProdutos(env), planos = await lerPlanos(env, PLANOS_CODIGO), licencas = await lerLicencas(env, user);
  const alvo = [prod, ...(prod.deps || []).map((d) => itens[d.id]).filter(Boolean)].filter((p) => p.licenciado || p === prod);
  for (const p of alvo) {
    if (p !== prod && !p.licenciado) continue;
    const a = autorizar({ rec, user, estado, prod: p, planos, licencas });
    if (!a.ok) return p === prod ? a.motivo : "requer " + p.nome + ": " + a.motivo;
  }
  return null;
}

// ---------- assinatura Ed25519 ----------
let chavePriv = null, chavePrivDe = null, chavePub = "";
async function chaveLic(env) {
  if (!env.LIC_PRIV) return null;
  if (chavePrivDe !== env.LIC_PRIV) {
    const der = Uint8Array.from(atob(env.LIC_PRIV.trim()), (c) => c.charCodeAt(0));
    const k = await crypto.subtle.importKey("pkcs8", der, { name: "Ed25519" }, true, ["sign"]);
    const jwk = await crypto.subtle.exportKey("jwk", k);
    chavePub = hex(Uint8Array.from(atob(jwk.x.replace(/-/g, "+").replace(/_/g, "/")), (c) => c.charCodeAt(0)));
    chavePriv = k; chavePrivDe = env.LIC_PRIV;
  }
  return chavePriv;
}
export async function emitirAutorizacao(env, campos) {
  const k = await chaveLic(env);
  if (!k) return null;
  const payload = ["JKL1", campos.modulo, campos.cid, campos.uh, campos.vmin || "-", campos.iat, campos.exp].join("|");
  const sig = hex(await crypto.subtle.sign({ name: "Ed25519" }, k, enc.encode(payload)));
  return payload + "|" + sig;
}
export async function lerLicfg(env) {
  return cache("licfg", async () => Object.assign(
    { dias: 7, tolerancia: 0, consoles_padrao: 1, tofu: false, tofu_ate: "", tofu_produtos: [] },
    await kvJson(env, "licfg", {})
  ));
}

// ---------- TOFU (trust-on-first-use): licenca geral na janela, depois individual ----------
// So se aplica quando a conta JA esta ativa e o motivo da negativa e puramente de plano
// (produto nao esta na lista do plano, ou o produto restringe planos). Nunca reabre
// licenca revogada/suspensa, conta suspensa/vencida ou produto bloqueado/versao incompativel.
const MOTIVOS_TOFU = ["produto fora do plano", "produto nao liberado para o plano"];
function tofuJanelaAberta(cfg) {
  return !!(cfg.tofu && cfg.tofu_ate && Date.parse(cfg.tofu_ate) > Date.now());
}
function tofuElegivel(cfg, prod) {
  const lista = cfg.tofu_produtos || [];
  return lista.includes("*") || lista.includes(prod.id);
}
async function concederTofu(env, { user, prod, cid, con, request, t }) {
  const doc = await kvJson(env, "lic:" + user, { v: 1, itens: [] });
  const itens = doc.itens || [];
  const ip = (request && request.headers.get("CF-Connecting-IP")) || "";
  const det = "console " + cid.slice(0, 8) + " modelo=" + ((con && con.modelo) || "-") + " fw=" + ((con && con.fw) || "-") + " ip=" + ip;
  itens.push({
    id: "tofu" + Date.now().toString(36) + rnd(), produto: prod.id, tipo: "permanente", status: "ativa",
    inicio: new Date(t * 1000).toISOString(), fim: "", consoles: 1, nota: ("TOFU 1o acesso: " + det).slice(0, 200)
  });
  await env.DB.put("lic:" + user, JSON.stringify({ v: 1, itens }));
  limpaCache("lic:" + user);
  await auditarAdmin(env, "sistema", "licenca TOFU concedida", user, prod.id + " - " + det);
  return { ok: true, ate: 0, via: "tofu" };
}

// ---------- consoles ----------
async function consolesDoUsuario(env, user) { return (await kvJson(env, "conu:" + user, [])) || []; }
async function gravarConsole(env, c) {
  await env.DB.put("con:" + c.cid, JSON.stringify(c), { metadata: { user: c.user, status: c.status, ultimo: c.ultimo, primeiro: c.primeiro, pais: c.pais || "" } });
}

// POST /lic/sync   (Authorization: Bearer <sessao do app>)
//   dev=<id do console>  mods=var:4.16,craque:1.0  fw=<opcional>  modelo=<opcional>
// Resposta texto (o app grava LIC em /data/jk/lic/<modulo>.lic e apaga os NEG):
//   OK
//   LIC\t<modulo>\t<autorizacao>
//   NEG\t<modulo>\t<motivo>
export async function licSync(request, env, user, rec, estado, deps) {
  const f = await deps.readForm(request);
  const dev = deps.cleanDev(f.dev);
  if (!dev) return texto(400, "ERRO\nconsole nao informado");
  if (!(await chaveLic(env))) return texto(503, "ERRO\nlicencas nao configuradas");
  const cfg = await lerLicfg(env);
  const cid = await cidDe(dev);
  const hd = await deps.hashDev(env, dev);
  const t = agora();
  const ip = request.headers.get("CF-Connecting-IP") || "", pais = (request.cf && request.cf.country) || "";
  const planos = await lerPlanos(env, deps.PLANOS);
  const plano = planos[rec.plan || ""];
  const limite = Math.max(1, Number((plano && plano.consoles) || cfg.consoles_padrao || 1));
  let con = await kvJson(env, "con:" + cid, null);
  const negar = async (motivo) => {
    await eventoSeguranca(env, "licenca-negada", user, request, motivo + " (console " + cid.slice(0, 8) + ")");
    const mods = String(f.mods || "").split(",").map((s) => s.split(":")[0].trim()).filter(Boolean);
    return texto(200, ["OK"].concat(mods.map((m) => "NEG\t" + m + "\t" + motivo)).join("\n"));
  };
  if (con && con.status === "revogado") return await negar("console revogado");
  if (con && con.user && con.user !== user) return await negar("console vinculado a outra conta");
  if (!con) {
    const meus = await consolesDoUsuario(env, user);
    const ativos = [];
    for (const c of meus) { const x = await kvJson(env, "con:" + c, null); if (x && x.status === "ativo" && x.user === user) ativos.push(c); }
    const doLogin = rec.dev && rec.dev === hd;                // o console que o login ja prendeu a conta sempre vale
    if (!doLogin && ativos.length >= limite) return await negar("limite de " + limite + " console(s) do plano");
    con = { cid, user, status: "ativo", primeiro: new Date().toISOString(), ultimo: "", ip: "", pais: "", fw: "", modelo: "", mods: {}, hist: [] };
    con.hist.push({ t: con.primeiro, ev: "vinculado", ip });
    await env.DB.put("conu:" + user, JSON.stringify([...new Set(meus.concat(cid))]));
    await auditarAdmin(env, "sistema", "console vinculado", user, cid);
  }
  const mods = {};
  for (const par of String(f.mods || "").split(",")) {
    const [m, v] = par.split(":").map((s) => (s || "").trim());
    if (okId(m || "")) mods[m] = (v || "").slice(0, 20);
  }
  const ultimoMs = Date.parse(con.ultimo || "") || 0;
  const mudou = con.ip !== ip || JSON.stringify(con.mods) !== JSON.stringify(mods) || (f.fw && con.fw !== String(f.fw).slice(0, 20));
  if (mudou || Date.now() - ultimoMs > 6 * 3600 * 1000) {     // grava no maximo a cada 6 h (ou quando algo muda)
    con.ultimo = new Date().toISOString(); con.ip = ip; con.pais = pais; con.mods = mods;
    if (f.fw) con.fw = String(f.fw).slice(0, 20);
    if (f.modelo) con.modelo = String(f.modelo).slice(0, 30);
    await gravarConsole(env, con);
  }
  const itens = await lerProdutos(env), licencas = await lerLicencas(env, user);
  // modulo licenciado = PRX propria (VAR) OU "modulo do Core" (PRX sem fonte, cifrada: tem chave_conteudo)
  const porModulo = {};
  for (const p of Object.values(itens)) if ((p.tipo === "PRX" || p.tipo === "CORE") && p.licenciado && p.modulo) porModulo[p.modulo] = p;
  const uh = await deps.idUsuario(env, user);
  const linhas = ["OK"];
  const indice = [];                                       // modulos do Core liberados -> /data/jk/lic/indice.txt
  const pedidos = Object.keys(mods).length ? Object.keys(mods) : Object.keys(porModulo);
  const tofuAberto = tofuJanelaAberta(cfg);
  for (const m of pedidos) {
    const prod = porModulo[m];
    if (!prod) { linhas.push("NEG\t" + m + "\tmodulo sem licenciamento"); continue; }
    let a = autorizar({ rec, user, estado, prod, planos, licencas, versao: mods[m] });
    if (!a.ok && tofuAberto && MOTIVOS_TOFU.includes(a.motivo) && tofuElegivel(cfg, prod)) {
      a = await concederTofu(env, { user, prod, cid, con, request, t });
      if (a.ok) licencas.push({ produto: prod.id, tipo: "permanente", status: "ativa" }); // demais modulos deste sync ja enxergam
    }
    if (!a.ok) { linhas.push("NEG\t" + m + "\t" + a.motivo); await eventoSeguranca(env, "licenca-negada", user, request, m + ": " + a.motivo); continue; }
    let exp = t + cfg.dias * DIA;
    if (a.ate) exp = Math.min(exp, a.ate + cfg.tolerancia * DIA);
    const tok = await emitirAutorizacao(env, { modulo: m, cid, uh, vmin: prod.versao_min, iat: t, exp });
    linhas.push("LIC\t" + m + "\t" + tok);
    if (prod.chave_conteudo && /^[0-9a-f]{64}$/.test(prod.chave_conteudo)) {   // modulo do Core: manda a chave e entra no indice
      linhas.push("KEY\t" + m + "\t" + prod.chave_conteudo);
      indice.push(m);
    }
  }
  if (indice.length || Object.values(porModulo).some((p) => p.chave_conteudo)) linhas.push("IDX\t" + indice.join(","));
  return texto(200, linhas.join("\n"));
}

// ---------- sessoes revogaveis ----------
// rec.sessoes_desde (ISO): token do app emitido antes disso deixa de valer (o app pede login de novo).
export function sessaoRevogada(rec, token, TOKEN_DAYS) {
  if (!rec || !rec.sessoes_desde) return false;
  try {
    const d = JSON.parse(new TextDecoder().decode(Uint8Array.from(atob(String(token).split(".")[0].replace(/-/g, "+").replace(/_/g, "/")), (c) => c.charCodeAt(0))));
    const emitido = (d.exp - TOKEN_DAYS * DIA) * 1000;
    return emitido < Date.parse(rec.sessoes_desde);
  } catch (e) { return true; }
}

// ---------- equipe do painel ----------
export const PAPEIS = ["super", "admin", "suporte", "operador"];
// o que cada papel pode fazer na central (o super pode tudo; so ele mexe em equipe, chaves e politicas)
const PODE = {
  admin: ["ler", "clientes", "consoles", "planos", "licencas", "produtos", "catalogo", "r2"],
  suporte: ["ler", "clientes", "consoles", "licencas"],
  operador: ["ler"]
};
export function pode(papel, acao) { return papel === "super" || (PODE[papel] || []).includes(acao); }

async function hashSenha(env, salt, senha) {
  const k = await crypto.subtle.importKey("raw", enc.encode(env.TOKEN_SECRET), { name: "HMAC", hash: "SHA-256" }, false, ["sign"]);
  return hex(await crypto.subtle.sign("HMAC", k, enc.encode("padm:" + salt + ":" + senha)));
}
export async function loginEquipe(env, user, senha) {
  const u = limpaUser(user);
  if (!okUser(u)) return null;
  const r = await kvJson(env, "padm:" + u, null);
  if (!r || r.ativo === false) return null;
  const h = await hashSenha(env, r.salt, String(senha || ""));
  let d = 0; for (let i = 0; i < Math.max(h.length, r.hash.length); i++) d |= (h.charCodeAt(i) || 0) ^ (r.hash.charCodeAt(i) || 0);
  return d === 0 ? { u, papel: PAPEIS.includes(r.papel) ? r.papel : "operador" } : null;
}

// ---------- API da central: /jkpainel17/api/c/... (ja autenticada; quem = {u, papel}) ----------
export async function centralApi(request, env, url, p, m, quem, deps) {
  const nega = () => json(403, { erro: "seu perfil (" + quem.papel + ") não pode fazer isso" });
  const aud = (acao, alvo, det) => auditarAdmin(env, quem.u, acao, alvo, det);
  if (!pode(quem.papel, "ler")) return nega();

  if (p === "c/eu" && m === "GET") return json(200, { u: quem.u, papel: quem.papel, licencas_configuradas: !!env.LIC_PRIV });

  // ---- dashboard ----
  if (p === "c/dashboard" && m === "GET") {
    const keys = await deps.listUsers(env);
    const est = { total: keys.length, ativo: 0, pendente: 0, vencido: 0, desativado: 0, vence7: 0 };
    const porMes = {};
    const t = Date.now();
    for (const k of keys) {
      const md = k.metadata || {};
      const st = deps.accountState({ disabled: md.disabled, pending: md.pending, expires: md.expires, v2: md.v2 });
      est[st] = (est[st] || 0) + 1;
      const e = Date.parse(md.expires || "");
      if (st === "ativo" && md.v2 && e && e - t < 7 * DIA * 1000) est.vence7++;
      const mes = String(md.created || "").slice(0, 7);
      if (mes) porMes[mes] = (porMes[mes] || 0) + 1;
    }
    const cons = await env.DB.list({ prefix: "con:", limit: 1000 });
    const consoles = { total: cons.keys.length, ativo: cons.keys.filter((k) => (k.metadata || {}).status === "ativo").length };
    const itens = Object.values(await lerProdutos(env));
    const produtos = { PRX: 0, CPK: 0, PKG: 0, CORE: 0, licenciados: 0 };
    for (const x of itens) { produtos[x.tipo] = (produtos[x.tipo] || 0) + 1; if (x.licenciado) produtos.licenciados++; }
    const lics = await env.DB.list({ prefix: "lic:", limit: 1000 });
    const dl = await env.DB.list({ prefix: "dl:", limit: 1000 });
    const porDia = {};
    for (const k of dl.keys) { const d = String((k.metadata || {}).t || "").slice(0, 10); if (d) porDia[d] = (porDia[d] || 0) + 1; }
    const ult = async (prefixo, n) => (await env.DB.list({ prefix: prefixo, limit: n })).keys.map((k) => k.metadata || {});
    return json(200, {
      clientes: est, clientes_por_mes: porMes, consoles, produtos, licencas_explicitas: lics.keys.length,
      downloads: { total_listado: dl.keys.length, por_dia: porDia }, ultimos_downloads: await ult("dl:", 10),
      ultimos_logins: await ult("log:", 10), seguranca: await ult("sec:", 10), auditoria: await ult("aud:", 10),
      licencas_configuradas: !!env.LIC_PRIV
    });
  }

  // ---- clientes ----
  if (p === "c/cliente" && m === "GET") {
    const u = limpaUser(url.searchParams.get("u"));
    const rec = await kvJson(env, "user:" + u, null);
    if (!rec) return json(404, { erro: "cliente não existe" });
    const seguro = Object.assign({}, rec); delete seguro.hash; delete seguro.salt; delete seguro.dev;
    const cids = await consolesDoUsuario(env, u);
    const consoles = [];
    for (const c of cids) { const x = await kvJson(env, "con:" + c, null); if (x) consoles.push(x); }
    const filtra = async (prefixo, n) => (await env.DB.list({ prefix: prefixo, limit: n })).keys.map((k) => k.metadata || {}).filter((x) => x.user === u || x.alvo === u);
    return json(200, { user: u, estado: deps.accountState(rec), conta: seguro, preso_a_console: !!rec.dev, consoles,
      licencas: await lerLicencas(env, u), logins: await filtra("log:", 1000), downloads: await filtra("dl:", 1000),
      seguranca: await filtra("sec:", 1000), auditoria: await filtra("aud:", 1000) });
  }
  if (p === "c/cliente/sessoes" && m === "POST") {
    if (!pode(quem.papel, "clientes")) return nega();
    const f = (await lerJson(request)) || {};
    const u = limpaUser(f.u);
    const rec = await kvJson(env, "user:" + u, null);
    if (!rec) return json(404, { erro: "cliente não existe" });
    rec.sessoes_desde = new Date().toISOString();
    await deps.putUser(env, u, rec, (await deps.userMeta(env, u)).last);
    await aud("sessões revogadas", u, "");
    return json(200, { ok: true });
  }

  // ---- consoles ----
  if (p === "c/consoles" && m === "GET") {
    const out = [];
    let cursor;
    do {
      const l = await env.DB.list({ prefix: "con:", cursor, limit: 1000 });
      for (const k of l.keys) out.push(Object.assign({ cid: k.name.slice(4) }, k.metadata || {}));
      cursor = l.list_complete ? undefined : l.cursor;
    } while (cursor && out.length < 5000);
    return json(200, { itens: out });
  }
  if (p === "c/console" && m === "GET") {
    const c = await kvJson(env, "con:" + String(url.searchParams.get("cid") || "").slice(0, 24), null);
    return c ? json(200, c) : json(404, { erro: "console não encontrado" });
  }
  if (p === "c/console" && m === "POST") {
    if (!pode(quem.papel, "consoles")) return nega();
    const f = (await lerJson(request)) || {};
    const cid = String(f.cid || "").slice(0, 24);
    const c = await kvJson(env, "con:" + cid, null);
    if (!c) return json(404, { erro: "console não encontrado" });
    const acao = String(f.acao || "");
    if (acao === "revogar" || acao === "reativar") {
      c.status = acao === "revogar" ? "revogado" : "ativo";
    } else if (acao === "desvincular" || acao === "transferir") {
      const lista = (await consolesDoUsuario(env, c.user)).filter((x) => x !== cid);
      await env.DB.put("conu:" + c.user, JSON.stringify(lista));
      if (acao === "transferir") {
        const novo = limpaUser(f.para);
        if (!(await kvJson(env, "user:" + novo, null))) return json(404, { erro: "conta de destino não existe" });
        await env.DB.put("conu:" + novo, JSON.stringify([...new Set((await consolesDoUsuario(env, novo)).concat(cid))]));
        c.user = novo; c.status = "ativo";
      } else { c.user = ""; c.status = "livre"; }
    } else return json(400, { erro: "ação inválida" });
    (c.hist = c.hist || []).push({ t: new Date().toISOString(), ev: acao + (f.para ? " para " + f.para : ""), por: quem.u });
    c.hist = c.hist.slice(-50);
    await gravarConsole(env, c);
    await aud("console: " + acao, c.user || cid, cid + (f.motivo ? " - " + String(f.motivo).slice(0, 100) : ""));
    return json(200, c);
  }

  // ---- planos ----
  if (p === "c/planos" && m === "GET") return json(200, { itens: await lerPlanos(env, deps.PLANOS) });
  if (p === "c/planos" && m === "PUT") {
    if (!pode(quem.papel, "planos")) return nega();
    const f = (await lerJson(request)) || {};
    const itens = {};
    for (const [id, x] of Object.entries(f.itens || {})) {
      if (!okId(id)) return json(400, { erro: "id de plano inválido: " + id });
      const dias = Math.floor(Number(x.dias) || 0);
      if (dias < 1 || dias > 3650) return json(400, { erro: "duração inválida no plano " + id });
      itens[id] = { id, nome: String(x.nome || id).slice(0, 40), preco: String(x.preco || "0").slice(0, 12), dias, pix: String(x.pix || "").slice(0, 400),
        consoles: Math.max(1, Math.min(10, Math.floor(Number(x.consoles) || 1))), produtos: Array.isArray(x.produtos) && x.produtos.length ? x.produtos.map(String).slice(0, 500) : ["*"],
        status: x.status === "inativo" ? "inativo" : "ativo", renovavel: x.renovavel !== false };
    }
    if (!Object.keys(itens).length) return json(400, { erro: "nenhum plano" });
    await env.DB.put("planos", JSON.stringify({ v: 1, itens }));
    limpaCache("planos");
    await aud("planos salvos", "", Object.keys(itens).join(", "));
    return json(200, { itens });
  }

  // ---- licencas ----
  if (p === "c/licencas" && m === "GET") {
    const u = url.searchParams.get("u");
    if (u) return json(200, { user: limpaUser(u), itens: await lerLicencas(env, limpaUser(u)) });
    const l = await env.DB.list({ prefix: "lic:", limit: 1000 });
    const out = [];
    for (const k of l.keys) { const us = k.name.slice(4); for (const x of await lerLicencas(env, us)) out.push(Object.assign({ user: us }, x)); }
    return json(200, { itens: out });
  }
  if (p === "c/licencas" && m === "PUT") {
    if (!pode(quem.papel, "licencas")) return nega();
    const f = (await lerJson(request)) || {};
    const u = limpaUser(f.u);
    if (!(await kvJson(env, "user:" + u, null))) return json(404, { erro: "cliente não existe" });
    const itens = [];
    for (const x of (f.itens || []).slice(0, 200)) {
      const tipo = TIPOS_LIC.includes(x.tipo) ? x.tipo : null, status = STATUS_LIC.includes(x.status) ? x.status : null;
      if (!tipo || !status) return json(400, { erro: "tipo/status inválido" });
      if (x.produto !== "*" && !okId(String(x.produto || ""))) return json(400, { erro: "produto inválido" });
      if (tipo !== "permanente" && !(Date.parse(x.fim || "") > 0)) return json(400, { erro: "licença " + tipo + " precisa de data de fim" });
      itens.push({ id: x.id && okId(x.id) ? x.id : "l" + Date.now().toString(36) + rnd(), produto: String(x.produto), tipo, status,
        inicio: x.inicio || new Date().toISOString(), fim: tipo === "permanente" ? "" : new Date(Date.parse(x.fim)).toISOString(),
        consoles: Math.max(1, Math.min(10, Math.floor(Number(x.consoles) || 1))), nota: String(x.nota || "").slice(0, 200) });
    }
    if (itens.length) await env.DB.put("lic:" + u, JSON.stringify({ v: 1, itens })); else await env.DB.delete("lic:" + u);
    limpaCache("lic:" + u);
    await aud("licenças salvas", u, itens.map((x) => x.produto + ":" + x.tipo + ":" + x.status).join(", "));
    return json(200, { itens });
  }
  if (p === "c/autorizacao" && m === "GET") {                 // simulador: o que o servidor responderia para o cliente
    const u = limpaUser(url.searchParams.get("u"));
    const rec = await kvJson(env, "user:" + u, null);
    if (!rec) return json(404, { erro: "cliente não existe" });
    const itens = await lerProdutos(env), planos = await lerPlanos(env, deps.PLANOS), lic = await lerLicencas(env, u);
    const estado = deps.accountState(rec);
    const out = Object.values(itens).filter((x) => x.tipo === "PRX" && x.licenciado).map((prod) =>
      Object.assign({ produto: prod.id, nome: prod.nome, modulo: prod.modulo }, autorizar({ rec, user: u, estado, prod, planos, licencas: lic })));
    return json(200, { user: u, estado, itens: out });
  }

  // ---- produtos e dependencias ----
  if (p === "c/produtos" && m === "GET") {               // a chave de conteudo nunca sai do servidor: so "tem ou nao tem"
    const itens = {};
    for (const [id, p2] of Object.entries(await lerProdutos(env))) itens[id] = Object.assign({}, p2, { chave_conteudo: undefined, tem_chave: !!p2.chave_conteudo });
    return json(200, { itens });
  }
  if (p === "c/produtos/importar" && m === "POST") {
    if (!pode(quem.papel, "produtos")) return nega();
    const cat = (await env.DB.get("catalog")) || "";
    const itens = Object.assign({}, await kvJson(env, "prod:all", { itens: {} }).then((d) => d.itens || {}));
    const r = importarDoCatalogo(cat, itens, deps.R2_PUBLICO);
    await gravarProdutos(env, itens);
    await aud("produtos importados do catálogo", "", r.novos.length + " novos, " + r.deps + " dependências");
    return json(200, { novos: r.novos.length, deps: r.deps, total: Object.keys(itens).length });
  }
  if (p === "c/produto" && m === "PUT") {
    if (!pode(quem.papel, "produtos")) return nega();
    const x = (await lerJson(request)) || {};
    if (!okId(String(x.id || ""))) return json(400, { erro: "id inválido" });
    if (!["PRX", "CPK", "PKG", "CORE"].includes(x.tipo)) return json(400, { erro: "tipo inválido" });
    // CORE = PRX sem código-fonte, cifrada (.jkm) e carregada pelo JK Core. Sempre licenciada; tem chave de conteúdo.
    const licenciavel = x.tipo === "PRX" || x.tipo === "CORE";
    const itens = Object.assign({}, await lerProdutos(env));
    const velho = itens[x.id] || {};
    // Se nao veio chave nova valida nesta chamada, mantem a que ja estava gravada (o campo na tela
    // fica em branco de proposito, por seguranca, entao salvar sem mexer nao pode apagar a chave).
    const chaveConteudo = x.tipo === "CORE" ? (/^[0-9a-f]{64}$/.test(String(x.chave_conteudo || "")) ? x.chave_conteudo : (velho.chave_conteudo || "")) : "";
    if (x.tipo === "CORE" && x.licenciado && !chaveConteudo) return json(400, { erro: "módulo do Core precisa da chave de conteúdo (64 hex, do cifra_modulo)" });
    const depsOk = [];
    for (const d of (x.deps || []).slice(0, 50)) {
      if (!itens[d.id] || d.id === x.id) return json(400, { erro: "dependência inexistente: " + d.id });
      depsOk.push({ id: d.id, min: String(d.min || "").slice(0, 20) });
    }
    // sem ciclos
    const visita = (id, cam) => { if (cam.includes(id)) return true; const p2 = id === x.id ? { deps: depsOk } : itens[id]; return (p2 && p2.deps || []).some((d) => visita(d.id, cam.concat(id))); };
    if (visita(x.id, [])) return json(400, { erro: "dependência circular" });
    itens[x.id] = { id: x.id, tipo: x.tipo, nome: String(x.nome || x.id).slice(0, 80), arquivo: String(x.arquivo || "").slice(0, 400),
      versao: String(x.versao || "").slice(0, 20), versao_min: String(x.versao_min || "").slice(0, 20), sha256: /^[0-9a-f]{64}$/.test(x.sha256 || "") ? x.sha256 : "",
      tamanho: Math.max(0, Number(x.tamanho) || 0), status: ["ativo", "bloqueado", "rascunho", "revogado"].includes(x.status) ? x.status : "ativo",
      deps: depsOk, planos: Array.isArray(x.planos) && x.planos.length ? x.planos.map(String).slice(0, 50) : ["*"],
      licenciado: licenciavel && (x.tipo === "CORE" ? true : !!x.licenciado), modulo: licenciavel && okId(String(x.modulo || "")) ? x.modulo : "",
      chave_conteudo: chaveConteudo,
      desc: String(x.desc || "").slice(0, 300), origem: velho.origem || [], criado: velho.criado || new Date().toISOString(), publicado: x.publicado || velho.publicado || "" };
    await gravarProdutos(env, itens);
    await aud(velho.id ? "produto alterado" : "produto criado", x.id, itens[x.id].tipo + " " + itens[x.id].status + (itens[x.id].licenciado ? " licenciado" : ""));
    return json(200, itens[x.id]);
  }
  if (p === "c/produto" && m === "DELETE") {
    if (!pode(quem.papel, "produtos")) return nega();
    const id = String(url.searchParams.get("id") || "");
    const itens = Object.assign({}, await lerProdutos(env));
    if (!itens[id]) return json(404, { erro: "produto não existe" });
    const usam = Object.values(itens).filter((y) => (y.deps || []).some((d) => d.id === id)).map((y) => y.id);
    if (usam.length) return json(400, { erro: "outros produtos dependem dele: " + usam.slice(0, 5).join(", ") });
    delete itens[id];
    await gravarProdutos(env, itens);
    await aud("produto removido do cadastro (arquivo no R2 continua)", id, "");
    return json(200, { ok: true });
  }
  if (p === "c/produto/hash" && m === "POST") {              // calcula SHA-256 e tamanho direto do R2 (arquivos ate 200 MB)
    if (!pode(quem.papel, "produtos")) return nega();
    const id = String(url.searchParams.get("id") || "");
    const itens = Object.assign({}, await lerProdutos(env));
    const x = itens[id];
    if (!x || !x.arquivo) return json(404, { erro: "produto sem arquivo" });
    const h = await env.APP_BUCKET.head(x.arquivo);
    if (!h) return json(404, { erro: "arquivo não está no R2" });
    if (h.size > 200 * 1024 * 1024) return json(400, { erro: "arquivo grande demais para calcular aqui (" + h.size + " bytes)" });
    const o = await env.APP_BUCKET.get(x.arquivo);
    x.sha256 = hex(await crypto.subtle.digest("SHA-256", await o.arrayBuffer()));
    x.tamanho = h.size;
    await gravarProdutos(env, itens);
    return json(200, x);
  }

  // ---- auditoria / seguranca / equipe / config ----
  if ((p === "c/auditoria" || p === "c/seguranca") && m === "GET") {
    const r = await env.DB.list({ prefix: p === "c/auditoria" ? "aud:" : "sec:", limit: 300, cursor: url.searchParams.get("cursor") || undefined });
    return json(200, { itens: r.keys.map((k) => k.metadata || {}), cursor: r.list_complete ? "" : r.cursor });
  }
  if (p === "c/config" && m === "GET") return json(200, { licfg: await lerLicfg(env), licencas_configuradas: !!env.LIC_PRIV, chave_publica: (await chaveLic(env)) ? chavePub : "" });
  if (p === "c/config" && m === "PUT") {
    if (quem.papel !== "super") return nega();
    const f = (await lerJson(request)) || {};
    let tofuAte = "";
    if (f.tofu_ate) {
      const d = new Date(f.tofu_ate);
      if (isNaN(d.getTime())) return json(400, { erro: "data do TOFU invalida" });
      tofuAte = d.toISOString();
    }
    const c = { dias: Math.max(1, Math.min(60, Math.floor(Number(f.dias) || 7))), tolerancia: Math.max(0, Math.min(30, Math.floor(Number(f.tolerancia) || 0))),
                consoles_padrao: Math.max(1, Math.min(10, Math.floor(Number(f.consoles_padrao) || 1))),
                tofu: !!f.tofu, tofu_ate: tofuAte,
                tofu_produtos: Array.isArray(f.tofu_produtos) ? f.tofu_produtos.map(String).slice(0, 200) : [] };
    await env.DB.put("licfg", JSON.stringify(c));
    limpaCache("licfg");
    await aud("política de licenças", "", JSON.stringify(c));
    return json(200, c);
  }
  if (p === "c/equipe" && m === "GET") {
    if (quem.papel !== "super") return nega();
    const l = await env.DB.list({ prefix: "padm:", limit: 200 });
    return json(200, { itens: l.keys.map((k) => Object.assign({ u: k.name.slice(5) }, k.metadata || {})) });
  }
  if (p === "c/equipe" && m === "PUT") {
    if (quem.papel !== "super") return nega();
    const f = (await lerJson(request)) || {};
    const u = limpaUser(f.u);
    if (!okUser(u)) return json(400, { erro: "usuário inválido" });
    if (!PAPEIS.includes(f.papel) || f.papel === "super") return json(400, { erro: "papel inválido (o superadministrador é o login principal)" });
    const velho = await kvJson(env, "padm:" + u, null);
    let salt = velho && velho.salt, hash = velho && velho.hash;
    if (f.senha) {
      if (String(f.senha).length < 10) return json(400, { erro: "senha com no mínimo 10 caracteres" });
      salt = hex(crypto.getRandomValues(new Uint8Array(12)));
      hash = await hashSenha(env, salt, String(f.senha));
    }
    if (!hash) return json(400, { erro: "informe a senha" });
    const r = { salt, hash, papel: f.papel, ativo: f.ativo !== false };
    await env.DB.put("padm:" + u, JSON.stringify(r), { metadata: { papel: r.papel, ativo: r.ativo } });
    await aud("equipe: " + (velho ? "alterado" : "criado"), u, r.papel + (r.ativo ? "" : " (desativado)"));
    return json(200, { u, papel: r.papel, ativo: r.ativo });
  }
  if (p === "c/equipe" && m === "DELETE") {
    if (quem.papel !== "super") return nega();
    const u = limpaUser(url.searchParams.get("u"));
    await env.DB.delete("padm:" + u);
    await aud("equipe: removido", u, "");
    return json(200, { ok: true });
  }
  return null;
}
