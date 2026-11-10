(() => {
  "use strict";
  const keys = /*BRAIN_ZERO_MESSAGE_KEYS*/[];
  const templates = /*BRAIN_ZERO_BACKEND_PATTERNS*/[];
  const {lang = "en", messages = []} = window.bzLocale || {};
  const catalogue = new Map(keys.map((key, index) => [key, messages[index] || key]));
  const normalize = text => text.trim().replace(/\s+/gu, " ");
  const escapeRegex = text => text.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
  const field = /\{(\w+)\}/g;
  const substitute = (template, values) => template.replace(field, (token, key) =>
    Object.hasOwn(values, key) ? String(values[key]) : token);
  const folded = new Map();
  for (const [key, value] of catalogue) {
    const lower = key.toLowerCase();
    if (!folded.has(lower)) folded.set(lower, value);
  }
  const patterns = templates.map(template => {
    const names = Array.from(template.matchAll(field), match => match[1]);
    const source = template.split(/\{\w+\}/).map(escapeRegex).join("(.+?)");
    return {template, names, expression: new RegExp("^" + source + "$", "u")};
  });

  function localize(text, depth = 0) {
    if (lang === "en" || typeof text !== "string") return text;
    const key = normalize(text);
    if (!key) return text;
    const exact = catalogue.get(key) ?? folded.get(key.toLowerCase());
    if (exact !== undefined) return exact;
    if (depth >= 5) return text;
    for (const {template, names, expression} of patterns) {
      const match = expression.exec(key);
      if (match) {
        const values = Object.fromEntries(names.map((name, index) =>
          [name, name.startsWith("_") ? match[index + 1] : localize(match[index + 1], depth + 1)]));
        return substitute(catalogue.get(template) || template, values);
      }
    }
    return text;
  }

  window.bzText = localize;
  window.bzFormat = (template, values = {}) => substitute(localize(template), values);
  document.documentElement.lang = lang;
  document.documentElement.dir = lang === "ar" ? "rtl" : "ltr";
  if (lang === "en") return;

  const excluded = "script,style,code,textarea,option[lang],[data-no-translate]";
  const attributes = ["aria-label", "aria-description", "aria-valuetext", "title", "placeholder", "alt"];
  const translated = new WeakMap();
  function translateText(node) {
    const parent = node.parentElement;
    const original = node.nodeValue;
    if (!parent || !original.trim() || parent.closest(excluded) || translated.get(node) === original) return;
    const text = normalize(original);
    const result = localize(text);
    if (text === result) return;
    // Labels without an explicit value otherwise change what a form submits.
    const option = parent.closest("option:not([value])");
    if (option) option.value = option.value;
    const value = original.match(/^\s*/u)[0] + result + original.match(/\s*$/u)[0];
    translated.set(node, value);
    node.nodeValue = value;
  }

  function translateAttributes(element) {
    if (element.closest(excluded)) return;
    for (const attribute of attributes) {
      const original = element.getAttribute(attribute);
      if (!original) continue;
      const result = localize(original);
      if (result !== original) element.setAttribute(attribute, result);
    }
  }

  function translateTree(node) {
    if (node.nodeType === Node.TEXT_NODE) {
      translateText(node);
      return;
    }
    if (node.nodeType !== Node.ELEMENT_NODE || node.closest(excluded)) return;
    translateAttributes(node);
    for (const element of node.querySelectorAll("[aria-label],[aria-description],[aria-valuetext],[title],[placeholder],[alt]")) {
      translateAttributes(element);
    }
    const walker = document.createTreeWalker(node, NodeFilter.SHOW_TEXT);
    while (walker.nextNode()) translateText(walker.currentNode);
  }

  function start() {
    translateTree(document.documentElement);
    new MutationObserver(records => {
      for (const record of records) {
        if (!record.target.isConnected) continue;
        if (record.type === "characterData") translateText(record.target);
        else if (record.type === "attributes") translateAttributes(record.target);
        else for (const node of record.addedNodes) if (node.isConnected) translateTree(node);
      }
    }).observe(document.documentElement, {
      subtree: true, childList: true, characterData: true, attributes: true, attributeFilter: attributes,
    });
  }
  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", start, {once: true});
  else start();
})();
