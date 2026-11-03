/*
 *  SPDX-FileCopyrightText: 2026 Francesco Magazzu
 *
 *  SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

.pragma library

// Built-in chat services. `prop` is the configuration key that shows the
// site in the header's site list. Shared by the widget and its settings.
const predefined = [
    { id: "chatgpt",     text: "ChatGPT",         url: "https://chatgpt.com",               prop: "showChatGPT" },
    { id: "claude",      text: "Claude",          url: "https://claude.ai/new",             prop: "showClaude" },
    { id: "google",      text: "Google Gemini",   url: "https://gemini.google.com/app",     prop: "showGoogleGemini" },
    { id: "deepseek",    text: "DeepSeek",        url: "https://chat.deepseek.com",         prop: "showDeepSeek" },
    { id: "duckduckgo",  text: "DuckDuckGo Chat", url: "https://duckduckgo.com/chat",       prop: "showDuckDuckGoChat" },
    { id: "copilot",     text: "Bing Copilot",    url: "https://copilot.microsoft.com/",    prop: "showBingCopilot" },
    { id: "perplexity",  text: "Perplexity",      url: "https://www.perplexity.ai",         prop: "showPerplexity" },
    { id: "grok",        text: "Grok",            url: "https://x.com/i/grok",              prop: "showGrok" },
    { id: "meta",        text: "Meta AI",         url: "https://www.meta.ai",               prop: "showMetaAI" },
    { id: "huggingface", text: "HuggingChat",     url: "https://huggingface.co/chat",       prop: "showHugginChat" },
    { id: "t3",          text: "T3 Chat",         url: "https://t3.chat",                   prop: "showT3Chat" },
    { id: "you",         text: "You",             url: "https://you.com/?chatMode=default", prop: "showYou" },
    { id: "blackbox",    text: "BlackBox AI",     url: "https://www.blackbox.ai",           prop: "showBlackBox" },
    { id: "lobechat",    text: "LobeChat",        url: "https://lobechat.com/chat",         prop: "showLobeChat" },
    { id: "bigagi",      text: "Big-AGI",         url: "https://get.big-agi.com",           prop: "showBigAGI" }
];

// "Name|url,Name|url" (the customSites setting) -> [{text, url}]
function parseCustom(value) {
    return (value || "").split(",")
        .filter(s => s && s.includes("|"))
        .map(s => {
            const i = s.indexOf("|");
            return { text: s.substring(0, i), url: s.substring(i + 1) };
        })
        .filter(s => s.text && s.url);
}

function serializeCustom(list) {
    return list.map(s => s.text.replace(/[|,]/g, " ") + "|" + s.url).join(",");
}

function sanitizeUrl(url) {
    url = (url || "").trim();
    if (!url)
        return "";
    return /^[a-z][a-z0-9+.-]*:\/\//i.test(url) ? url : "https://" + url;
}
