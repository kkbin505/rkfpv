// UI text: every word the configurator shows lives in i18n/<lang>.json, not
// in the code. en.json is the reference; another language may leave keys
// out and they show in English. configurator/check_i18n.py finds keys that
// are missing, unused or untranslated.
//
//   t("toast.saved", { n: 3 })   text for a key, {n} filled in
//   tf("param.fps", p.help)      the same, with a fallback for keys that may
//                                not exist (setting help comes from the board)
//   applyDom()                   fills data-i18n / data-i18n-html /
//                                data-i18n-title / data-i18n-placeholder
export const LANGS = { en: "EN", zh: "中文" };

let en = {};
let cur = {};
export let lang = "en";

async function load(code) {
  const r = await fetch(`i18n/${code}.json`, { cache: "no-cache" });
  if (!r.ok) throw new Error(`i18n/${code}.json: HTTP ${r.status}`);
  return r.json();
}

// the saved choice, else the browser's language
export function initialLang() {
  let saved = null;
  try { saved = localStorage.getItem("rkfpv.lang"); } catch (e) { /* private mode */ }
  if (saved && saved in LANGS) return saved;
  return /^zh\b/i.test(navigator.language || "") ? "zh" : "en";
}

export async function setLang(code) {
  if (!(code in LANGS)) code = "en";
  if (!Object.keys(en).length) en = await load("en");
  try {
    cur = code === "en" ? en : await load(code);
  } catch (e) {
    console.warn(e);
    cur = en;
    code = "en";
  }
  lang = code;
  document.documentElement.lang = code === "zh" ? "zh-CN" : code;
  try { localStorage.setItem("rkfpv.lang", code); } catch (e) { /* private mode */ }
}

const fill = (s, vars) => (vars ? s.replace(/\{(\w+)\}/g, (m, k) => (k in vars ? String(vars[k]) : m)) : s);

export function tf(key, fallback, vars) {
  const s = cur[key] ?? en[key];
  return fill(s === undefined ? fallback : s, vars);
}

export function t(key, vars) {
  return tf(key, key, vars);
}

export function applyDom(root = document) {
  for (const el of root.querySelectorAll("[data-i18n]")) el.textContent = t(el.dataset.i18n);
  for (const el of root.querySelectorAll("[data-i18n-html]")) el.innerHTML = t(el.dataset.i18nHtml);
  for (const el of root.querySelectorAll("[data-i18n-title]")) el.title = t(el.dataset.i18nTitle);
  for (const el of root.querySelectorAll("[data-i18n-placeholder]")) el.placeholder = t(el.dataset.i18nPlaceholder);
}
