// JK PATCHS - D1: DOWNLOAD PROTEGIDO PELO WORKER
//
// O /catalog troca os links publicos do R2 (pub-...r2.dev) por links deste Worker:
//     https://<host>/f/<token 32>/<nome-do-arquivo>?<query original (once, as, list)>
// token = id do arquivo (8) + id do usuario (6) + validade base36 (6) + assinatura (12)
//   - o id do arquivo e um HMAC da chave do R2: a chave real nao aparece no link
//   - a assinatura (TOKEN_SECRET) prende usuario + validade: ninguem gera link para outra conta
//     nem estica a validade; o arquivo so e entregue se estiver publicado no catalogo (fidmap)
//   - o link vale LINK_DIAS dias e so funciona enquanto a conta estiver ativa
// /f/ entrega o arquivo do R2 (binding APP_BUCKET) com suporte a Range (PKG grande e DPI).
//
// Liga/desliga sem deploy (painel > Downloads): KV "cfg:links"
//   {"modo":"off"|"lista"|"todos","usuarios":["joao"],"auditoria":"kv"|"log"}
//   off   = catalogo igual ao de hoje (links publicos)
//   lista = so os usuarios da lista recebem links protegidos (para testar)
//   todos = todo cliente ativo
// Chaves novas no KV (nada existente e alterado):
//   cfg:links       configuracao acima
//   fidmap          {"h": versao, "m": {id: chave_do_R2}}
//   uh:<id>         id curto do usuario -> nome do usuario
//   dl:<ts>:<id>    evento de download (metadados, expira em 90 dias)

const enc = new TextEncoder();
export const R2_PUBLICO = "https://pub-c6518eb1477949a58d959f5a769703d1.r2.dev/";
const EXT_PROTEGIDA = new Set(["prx", "cpk", "pkg", "bin", "ini", "json", "pcc", "txt", "lst", "dat", "aes", "sfo"]);
const SEM_EXTENSAO = new Set(["keystone", "sce_paramsfo1", "sce_icon0png1"]);
const LINK_DIAS = 30;
const DL_TTL = 60 * 60 * 24 * 90;
const CACHE_MS = 60 * 1000;
const RX_R2 = new RegExp(R2_PUBLICO.replace(/[.*+?^${}()|[\]\\/]/g, "\\$&") + "[^\\t|\\r\\n]*", "g");
const RX_TOKEN = /^[A-Za-z0-9_-]{8}[A-Za-z0-9_-]{6}[0-9a-z]{6}[A-Za-z0-9_-]{12}$/;

// ---------- cripto ----------
let chaveMemo = null, chaveDe = null;
async function chave(env) {
  if (chaveDe !== env.TOKEN_SECRET) {
    chaveMemo = await crypto.subtle.importKey("raw", enc.encode(env.TOKEN_SECRET), { name: "HMAC", hash: "SHA-256" }, false, ["sign"]);
    chaveDe = env.TOKEN_SECRET;
  }
  return chaveMemo;
}
function b64u(buf) {
  let s = "";
  for (const b of new Uint8Array(buf)) s += String.fromCharCode(b);
  return btoa(s).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
}
async function assina(env, txt, n) {
  return b64u(await crypto.subtle.sign("HMAC", await chave(env), enc.encode(txt))).slice(0, n);
}
function igual(a, b) {
  if (a.length !== b.length) return false;
  let d = 0;
  for (let i = 0; i < a.length; i++) d |= a.charCodeAt(i) ^ b.charCodeAt(i);
  return d === 0;
}
const idMemo = new Map();                                   // chave do R2 -> id (nao recalcula a cada catalogo)
const B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
// Id do arquivo: so esconde a chave do R2 (quem protege o link e a assinatura). Hash rapido e sincrono
// com o TOKEN_SECRET misturado: 48 bits, sem custo de CPU mesmo com centenas de arquivos (plano gratuito).
function hash48(s) {
  let h1 = 0xdeadbeef, h2 = 0x41c6ce57;
  for (let i = 0; i < s.length; i++) {
    const c = s.charCodeAt(i);
    h1 = Math.imul(h1 ^ c, 2654435761);
    h2 = Math.imul(h2 ^ c, 1597334677);
  }
  h1 = Math.imul(h1 ^ (h1 >>> 16), 2246822507) ^ Math.imul(h2 ^ (h2 >>> 13), 3266489909);
  h2 = Math.imul(h2 ^ (h2 >>> 16), 2246822507) ^ Math.imul(h1 ^ (h1 >>> 13), 3266489909);
  let id = "";
  for (let i = 0; i < 4; i++) id += B64[(h1 >>> (i * 6)) & 63];
  for (let i = 0; i < 4; i++) id += B64[(h2 >>> (i * 6)) & 63];
  return id;
}
export async function idArquivo(env, chaveR2) {
  let id = idMemo.get(chaveR2);
  if (!id) { id = hash48(env.TOKEN_SECRET + "|fid|" + chaveR2); idMemo.set(chaveR2, id); }
  return id;
}
// Assinatura do link: uma por usuario+validade. O arquivo e conferido no fidmap (so arquivos
// publicados no catalogo). Assim o catalogo custa 1 assinatura, nao 1 por arquivo (limite de CPU).
const assinaLink = (env, uh, exp) => assina(env, "dl:" + uh + exp, 12);
export const idUsuario = (env, user) => assina(env, "uh:" + user, 6);

// ---------- configuracao ----------
const CFG_PADRAO = { modo: "off", usuarios: [], auditoria: "log" };   // "log": nao gasta gravacoes do KV (plano gratuito)
let cfgMemo = null, cfgEm = 0;
export async function lerCfg(env, fresco) {
  if (!fresco && cfgMemo && Date.now() - cfgEm < CACHE_MS) return cfgMemo;
  let c = null;
  try { c = await env.DB.get("cfg:links", "json"); } catch (e) {}
  cfgMemo = Object.assign({}, CFG_PADRAO, c || {});
  cfgEm = Date.now();
  return cfgMemo;
}

// ---------- links ----------
function partes(url) {
  const resto = url.slice(R2_PUBLICO.length);
  const q = resto.indexOf("?");
  const caminho = q < 0 ? resto : resto.slice(0, q);
  const query = q < 0 ? "" : resto.slice(q + 1);
  const nome = caminho.slice(caminho.lastIndexOf("/") + 1);
  let chaveR2 = null;
  try { chaveR2 = decodeURIComponent(caminho); } catch (e) {}
  const p = nome.lastIndexOf(".");
  const ext = p > 0 ? nome.slice(p + 1).toLowerCase() : "";
  return { chaveR2, nome, query, protege: !!chaveR2 && !!nome && (EXT_PROTEGIDA.has(ext) || SEM_EXTENSAO.has(nome)) };
}

let mapaMemo = null, mapaEm = 0;
async function lerMapa(env, fresco) {
  if (!fresco && mapaMemo && Date.now() - mapaEm < CACHE_MS) return mapaMemo;
  let m = null;
  try { m = await env.DB.get("fidmap", "json"); } catch (e) {}
  mapaMemo = m && m.m ? m : { h: "", m: {} };
  mapaEm = Date.now();
  return mapaMemo;
}
async function garantirMapa(env, novos) {
  const ids = Object.keys(novos).sort();
  const atual = await lerMapa(env);
  if (ids.every((i) => atual.m[i] === novos[i])) return;
  const fresco = await lerMapa(env, true);
  if (ids.every((i) => fresco.m[i] === novos[i])) return;
  const m = Object.assign({}, fresco.m, novos);           // mantem os antigos: link ja entregue continua valendo
  const h = String(Date.now());
  await env.DB.put("fidmap", JSON.stringify({ h, m }));
  mapaMemo = { h, m };
  mapaEm = Date.now();
}

const uhGravados = new Set();
async function garantirUsuario(env, uh, user) {
  if (uhGravados.has(uh)) return;
  const v = await env.DB.get("uh:" + uh);
  if (v !== user) await env.DB.put("uh:" + uh, user);
  uhGravados.add(uh);
}

// Devolve o catalogo com os links protegidos (ou o mesmo texto, se o modo nao se aplica a este usuario).
// Molde do catalogo: o texto cortado nos links protegidos. Feito 1x por versao do catalogo (fica em memoria);
// cada pedido so encaixa "origem/f/<id><usuario><validade><assinatura>/" nos cortes (limite de CPU do Worker).
let moldeDe = null, molde = null;
async function montarMolde(env, body) {
  if (moldeDe === body && molde) return molde;
  if (!idMemo.size) {                                      // ids ja salvos no KV: evita recalcular 300+ assinaturas
    const salvo = await lerMapa(env);
    for (const [id, k] of Object.entries(salvo.m)) idMemo.set(k, id);
  }
  const pedacos = [], cortes = [], ids = {};
  let ultimo = 0;
  const linhaApp = /^(APP|MOD)\t/;
  let pos = 0;
  for (const linha of body.split("\n")) {                  // linhas APP e MOD (o app e o modulo) ficam publicas
    if (!linhaApp.test(linha)) {
      RX_R2.lastIndex = 0;
      let mm;
      while ((mm = RX_R2.exec(linha))) {
        const p = partes(mm[0]);
        if (!p.protege) continue;
        const id = await idArquivo(env, p.chaveR2);
        ids[id] = p.chaveR2;
        pedacos.push(body.slice(ultimo, pos + mm.index));
        cortes.push({ id, resto: "/" + p.nome + (p.query ? "?" + p.query : "") });
        ultimo = pos + mm.index + mm[0].length;
      }
    }
    pos += linha.length + 1;
  }
  pedacos.push(body.slice(ultimo));
  molde = { pedacos, cortes, ids };
  moldeDe = body;
  return molde;
}

export async function reescreverCatalogo(env, request, user, body) {
  const cfg = await lerCfg(env);
  if (cfg.modo === "off") return body;
  if (cfg.modo === "lista" && !(cfg.usuarios || []).includes(user)) return body;
  const md = await montarMolde(env, body);
  if (!md.cortes.length) return body;
  await garantirMapa(env, md.ids);
  const uh = await idUsuario(env, user);
  await garantirUsuario(env, uh, user);
  const exp = (Math.floor(Date.now() / 1000) + LINK_DIAS * 86400).toString(36).padStart(6, "0");
  const meio = new URL(request.url).origin + "/f/";
  const fimTok = uh + exp + (await assinaLink(env, uh, exp));
  const out = new Array(md.pedacos.length * 2 - 1);
  for (let i = 0; i < md.cortes.length; i++) {
    out[2 * i] = md.pedacos[i];
    out[2 * i + 1] = meio + md.cortes[i].id + fimTok + md.cortes[i].resto;
  }
  out[out.length - 1] = md.pedacos[md.pedacos.length - 1];
  return out.join("");
}

// ---------- entrega ----------
function erro(status, msg) {
  return new Response(msg + "\n", { status, headers: { "Content-Type": "text/plain; charset=utf-8", "Cache-Control": "no-store" } });
}
function lerRange(h) {
  if (!h) return null;
  const m = /^bytes=(\d*)-(\d*)$/.exec(h.trim());
  if (!m || (m[1] === "" && m[2] === "")) return { invalido: true };
  if (m[1] === "") return { suffix: Number(m[2]) };
  const offset = Number(m[1]);
  return m[2] === "" ? { offset } : { offset, length: Number(m[2]) - offset + 1 };
}

const contaMemo = new Map();                                // uh -> {ok, user, em}
async function contaAtiva(env, uh, accountState) {
  const c = contaMemo.get(uh);
  if (c && Date.now() - c.em < CACHE_MS) return c;
  const user = await env.DB.get("uh:" + uh);
  const rec = user ? await env.DB.get("user:" + user, "json") : null;
  const estado = rec ? accountState(rec) : "";
  const r = { ok: estado === "ativo", user: user || "", rec, estado, em: Date.now() };
  contaMemo.set(uh, r);
  return r;
}

async function auditar(env, cfg, ev) {
  console.log(JSON.stringify(Object.assign({ evento: "download" }, ev)));
  if (cfg.auditoria !== "kv") return;
  const k = "dl:" + String(9999999999999 - Date.now()).padStart(13, "0") + ":" + ev.uh + ":" + Math.random().toString(36).slice(2, 6);
  try { await env.DB.put(k, "1", { metadata: ev, expirationTtl: DL_TTL }); } catch (e) {}
}

// GET/HEAD /f/<token>/<nome>
// checar(chaveR2, user, rec, estado) -> null (pode) ou motivo: regras de produto/dependencia da central de licencas
export async function servirArquivo(request, env, url, accountState, ctx, checar) {
  const m = request.method;
  if (m !== "GET" && m !== "HEAD") return erro(405, "metodo nao permitido");
  const resto = url.pathname.slice(3);
  const barra = resto.indexOf("/");
  const tk = barra < 0 ? resto : resto.slice(0, barra);
  const nome = barra < 0 ? "" : resto.slice(barra + 1);
  if (!RX_TOKEN.test(tk) || !nome) return erro(404, "nao encontrado");
  const id = tk.slice(0, 8), uh = tk.slice(8, 14), exp = tk.slice(14, 20), sig = tk.slice(20);
  if (!igual(sig, await assinaLink(env, uh, exp))) return erro(403, "link invalido");
  if (parseInt(exp, 36) < Math.floor(Date.now() / 1000)) return erro(410, "link expirado: abra o app de novo para atualizar");
  let mapa = await lerMapa(env);
  let chaveR2 = mapa.m[id];
  if (!chaveR2) { mapa = await lerMapa(env, true); chaveR2 = mapa.m[id]; }
  if (!chaveR2) return erro(404, "nao encontrado");
  let nomeReal = "", nomePedido = "";
  try { nomeReal = chaveR2.slice(chaveR2.lastIndexOf("/") + 1); nomePedido = decodeURIComponent(nome); } catch (e) {}
  if (nomeReal !== nomePedido) return erro(404, "nao encontrado");

  const range = lerRange(request.headers.get("Range"));
  if (range && range.invalido) return erro(416, "range invalido");
  const primeiro = !range || range.offset === 0;
  if (primeiro) {                                            // conta conferida no inicio de cada download
    const c = await contaAtiva(env, uh, accountState);
    if (!c.ok) return erro(403, "plano inativo: renove para baixar");
    if (checar) {
      const motivo = await checar(chaveR2, c.user, c.rec, c.estado);
      if (motivo) return erro(403, "download negado: " + motivo);
    }
  }

  if (m === "HEAD") {
    const h = await env.APP_BUCKET.head(chaveR2);
    if (!h) return erro(404, "nao encontrado");
    return new Response(null, { status: 200, headers: {
      "Content-Type": "application/octet-stream", "Content-Length": String(h.size), "Accept-Ranges": "bytes",
      "ETag": h.httpEtag, "Cache-Control": "private, no-store" } });
  }

  let obj, total, ini = 0, fim = 0;
  if (range) {                                               // mede antes: o pedaco pedido pode passar do fim do arquivo
    const h = await env.APP_BUCKET.head(chaveR2);
    if (!h) return erro(404, "nao encontrado");
    total = h.size;
    ini = range.suffix !== undefined ? Math.max(0, total - range.suffix) : range.offset;
    fim = range.suffix !== undefined || range.length === undefined ? total - 1 : Math.min(total, ini + range.length) - 1;
    if (ini >= total || fim < ini) {
      return new Response(null, { status: 416, headers: { "Content-Range": "bytes */" + total, "Cache-Control": "no-store" } });
    }
    obj = await env.APP_BUCKET.get(chaveR2, { range: { offset: ini, length: fim - ini + 1 } });
  } else obj = await env.APP_BUCKET.get(chaveR2);
  if (!obj) return erro(404, "nao encontrado");
  if (!range) total = obj.size;
  const hd = { "Content-Type": "application/octet-stream", "Accept-Ranges": "bytes", "ETag": obj.httpEtag, "Cache-Control": "private, no-store" };
  let status = 200;
  if (range) {
    hd["Content-Range"] = "bytes " + ini + "-" + fim + "/" + total;
    hd["Content-Length"] = String(fim - ini + 1);
    status = 206;
  } else hd["Content-Length"] = String(total);

  if (primeiro) {
    const cfg = await lerCfg(env);
    const c = await contaAtiva(env, uh, accountState);
    const ev = { uh, user: c.user, arquivo: nomeReal, tamanho: total, ip: request.headers.get("CF-Connecting-IP") || "",
                 pais: (request.cf && request.cf.country) || "", t: new Date().toISOString() };
    const p = auditar(env, cfg, ev);
    if (ctx && ctx.waitUntil) ctx.waitUntil(p); else await p;
  }
  return new Response(obj.body, { status, headers: hd });
}

// ---------- painel (/jkpainel17/api/links e /downloads), ja autenticado ----------
export async function painelLinks(request, env, url, p, m, registrar) {
  if (p === "links" && m === "GET") return json(200, await lerCfg(env, true));
  if (p === "links" && m === "PUT") {
    let f = {};
    try { f = await request.json(); } catch (e) { return json(400, { erro: "JSON inválido" }); }
    if (!["off", "lista", "todos"].includes(f.modo)) return json(400, { erro: "modo inválido" });
    const usuarios = (Array.isArray(f.usuarios) ? f.usuarios : []).map((u) => String(u).trim().toLowerCase())
      .filter((u) => /^[a-z0-9_.-]{2,32}$/.test(u)).slice(0, 200);
    const cfg = { modo: f.modo, usuarios, auditoria: f.auditoria === "kv" ? "kv" : "log" };
    await env.DB.put("cfg:links", JSON.stringify(cfg));
    cfgMemo = cfg; cfgEm = Date.now();
    if (registrar) await registrar(env, "downloads protegidos: modo " + cfg.modo + (cfg.modo === "lista" ? " (" + usuarios.join(", ") + ")" : ""), []);
    return json(200, cfg);
  }
  if (p === "downloads" && m === "GET") {
    const cursor = url.searchParams.get("cursor") || undefined;
    const r = await env.DB.list({ prefix: "dl:", limit: 200, cursor });
    return json(200, { items: r.keys.map((k) => k.metadata || {}), cursor: r.list_complete ? "" : r.cursor });
  }
  return null;
}
function json(status, o) {
  return new Response(JSON.stringify(o), { status, headers: { "Content-Type": "application/json; charset=utf-8", "Cache-Control": "no-store" } });
}
