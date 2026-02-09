let current = null;
const $ = id => document.getElementById(id);
const esc = s => String(s ?? "").replace(/[&<>"']/g, m => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[m]));
const countAllSyms = () => data.reduce((n, m) => n + (m.symbols ? m.symbols.length : 0), 0);

function highlight(code) {
    if (!code) return "";
    const rules = [
        { name: "str", regex: /(?:f?"(?:\\.|[^"])*"|f?'(?:\\.|[^'])*')/ },
        { name: "com", regex: /(?:;[^\n]*|\/\/.*)/ },
        { name: "kw", regex: /\b(?:fn|def|if|else|elif|while|for|return|use|import|export|module|case|break|continue|asm|const|let|mut|struct|enum|match|type|ptr|null|true|false|defer|in|as|and|or|not|try|catch|throw|finally)\b/ },
        { name: "num", regex: /\b(?:0x[0-9a-fA-F]+|[0-9]+(?:\.[0-9]+)?)\b/ },
        { name: "call", regex: /\b(?:[a-zA-Z_][a-zA-Z0-9_]*)(?=\s*\()/ }
    ];
    const full = new RegExp(rules.map(r => "(" + r.regex.source + ")").join("|"), "g");
    let lastIdx = 0, out = "", m;
    while ((m = full.exec(code)) !== null) {
        out += esc(code.substring(lastIdx, m.index));
        for (let i = 0; i < rules.length; i++) if (m[i + 1] !== undefined) { out += `<span class="syn-${rules[i].name}">${esc(m[i + 1])}</span>`; break; }
        lastIdx = full.lastIndex;
    }
    return out + esc(code.substring(lastIdx));
}

function highlightBash(code) {
    if (!code) return "";
    const rules = [
        { name: "com", regex: /(?:#[^\n]*)/ },
        { name: "str", regex: /(?:'(?:\\.|[^'])*'|"(?:\\.|[^"])*")/ },
        { name: "var", regex: /(?:\$\{[^}]+\}|\$[A-Za-z_][A-Za-z0-9_]*)/ },
        { name: "kw", regex: /\b(?:if|then|fi|elif|else|for|while|do|done|case|esac|in|function|export|return|exit)\b/ },
        { name: "num", regex: /\b\d+\b/ },
        { name: "call", regex: /\b(?:sudo|apt|apt-get|pacman|dnf|yum|brew|docker|podman|make|cmake|ninja|git|curl|wget|python3|python|pip|pip3|bash|sh|zsh|chmod|chown|tar|grep|rg|sed|awk)\b/ }
    ];
    const full = new RegExp(rules.map(r => "(" + r.regex.source + ")").join("|"), "g");
    let lastIdx = 0, out = "", m;
    while ((m = full.exec(code)) !== null) {
        out += esc(code.substring(lastIdx, m.index));
        for (let i = 0; i < rules.length; i++) if (m[i + 1] !== undefined) { out += `<span class="syn-${rules[i].name}">${esc(m[i + 1])}</span>`; break; }
        lastIdx = full.lastIndex;
    }
    return out + esc(code.substring(lastIdx));
}

function renderRichDocstring(text) {
    if (!text) return "";
    let html = esc(text);
    html = html.replace(/\*\*([^*]+)\*\*/g, "<b>$1</b>")
        .replace(/\*([^*]+)\*/g, "<i>$1</i>")
        .replace(/`([^`]+)`/g, "<code>$1</code>")
        .replace(/^\s*[-*]\s+(.*)$/gm, "<ul><li>$1</li></ul>")
        .replace(/<\/ul>\n<ul>/g, "\n")
        .replace(/\[\[([a-zA-Z0-9_.:(\), ]+)\]\]/g, (match, p1) => {
            const parts = p1.split("::");
            if (parts.length > 1) return `<a href="#${p1}" onclick="selectModule('${parts[0]}', '${parts[1]}');event.preventDefault();">${esc(p1)}</a>`;
            return `<a href="#${p1}" onclick="selectModule('${p1}');event.preventDefault();">${esc(p1)}</a>`;
        });
    return html;
}

const getArgs = sig => (sig || "").match(/\((.*)\)/)?.[1]?.trim() || "";

function copyText(t, btn) {
    navigator.clipboard.writeText(String(t ?? "")).then(() => {
        const old = btn.textContent; btn.textContent = "OK";
        setTimeout(() => { btn.textContent = old; }, 1000);
    });
}

function toggleNavCategory(categoryId) {
    const categoryElement = $(categoryId);
    const gtitleElement = categoryElement.previousElementSibling;
    if (categoryElement.classList.contains("active")) { categoryElement.classList.remove("active"); gtitleElement.classList.remove("active"); }
    else { categoryElement.classList.add("active"); gtitleElement.classList.add("active"); }
}

function toggleNavModule(moduleId) {
    const moduleElement = $(moduleId);
    const gtitleElement = moduleElement.previousElementSibling;
    if (moduleElement.classList.contains("active")) { moduleElement.classList.remove("active"); gtitleElement.classList.remove("active"); }
    else { moduleElement.classList.add("active"); gtitleElement.classList.add("active"); }
}

function toggleOverviewModule(moduleId) {
    const moduleElement = $(moduleId);
    const cardRowElement = moduleElement.previousElementSibling;
    if (moduleElement.classList.contains("active")) { moduleElement.classList.remove("active"); cardRowElement.classList.remove("active"); }
    else { moduleElement.classList.add("active"); cardRowElement.classList.add("active"); }
}

function toggleSidebar() {
    const asideElement = $("aside"), mainElement = $("content-area"), backdrop = $("backdrop"), hamburger = $("hamburger");
    if (asideElement && mainElement) {
        const isActive = asideElement.classList.toggle("active");
        mainElement.classList.toggle("aside-active");
        if (backdrop) backdrop.classList.toggle("active", isActive);
        if (hamburger) hamburger.classList.toggle("active", isActive);
        if (!isActive) { const searchInput = $("search"); if (searchInput) searchInput.value = ""; }
    }
}

function renderNav() {
    const navTree = {};
    data.forEach(m => {
        let currentNode = navTree;
        m.path.forEach((component, index) => {
            if (!currentNode[component]) currentNode[component] = { _modules: [] };
            if (index === m.path.length - 1) currentNode[component]._modules.push(m);
            currentNode = currentNode[component];
        });
    });

    const overviewMod = data.find(m => m.name === "Overview");
    let mdDocs = overviewMod ? overviewMod.markdown_docs : [];
    mdDocs.sort((docA, docB) => {
        const priority = ["README", "LANGUAGE", "NY", "NYTRIX", "PROPOSALS", "DOCKER", "TODO"];
        const a = docA.name, b = docB.name, ia = priority.indexOf(a), ib = priority.indexOf(b);
        if (ia !== -1 && ib !== -1) return ia - ib;
        if (ia !== -1) return -1;
        if (ib !== -1) return 1;
        return a.localeCompare(b);
    });
    const infoDocs = mdDocs.filter(d => d.name === "NY" || d.name === "NYTRIX");
    const miscDocs = mdDocs.filter(d => d.name !== "NY" && d.name !== "NYTRIX");

    function renderInfoSection(depth) {
        if (!infoDocs.length) return "";
        const infoId = `nav-info-${depth}`;
        const infoActive = true;
        let html = "";
        html += `<div class="gtitle collapsible ${infoActive ? "active" : ""}" style="padding-left: ${12 + depth * 10}px;" onclick="toggleNavCategory('${infoId}')">INFO <span class="collapse-icon ${infoActive ? "active" : ""}">▶</span></div>`;
        html += `<div id="${infoId}" class="collapsible-content ${infoActive ? "active" : ""}">`;
        infoDocs.forEach(doc => {
            const itemActive = current === doc.name;
            html += `<a class="item ${itemActive ? "active" : ""}" href="#${doc.name}" onclick="selectModule('${doc.name.replace(/'/g, "\\'")}', '${doc.name}');event.preventDefault();" style="padding-left: ${12 + (depth + 1) * 10}px;">${esc(doc.name)}</a>`;
        });
        html += `</div>`;
        return html;
    }

    function renderTree(node, depth = 0, currentPath = "") {
        let nodeHtml = "";
        const sortedKeys = Object.keys(node).sort((a, b) => {
            const priority = ["Home", "README", "LANGUAGE", "NY", "NYTRIX", "PROPOSALS", "DOCKER", "TODO"];
            const ia = priority.indexOf(a), ib = priority.indexOf(b);
            if (ia !== -1 && ib !== -1) return ia - ib;
            if (ia !== -1) return -1;
            if (ib !== -1) return 1;
            return a.localeCompare(b);
        });

        sortedKeys.forEach(key => {
            if (key === "_modules") return;

            const newPath = currentPath ? `${currentPath}.${key}` : key;
            const currentCategory = node[key];
            const categoryId = `nav-cat-${newPath.replace(/[^a-zA-Z0-9]/g, "_")}-${depth}`;
            const isPathActive = current && (current === newPath || current.startsWith(newPath + ".") || current.startsWith(newPath + "::"));

            if (key === "Home") {
                const isActive = (depth === 0) || (current === "Overview") || (overviewMod && overviewMod.markdown_docs.some(d => d.name === current));
                const hasHomeDocs = miscDocs.length > 0 || infoDocs.length > 0;
                nodeHtml += `<div class="gtitle ${hasHomeDocs ? "collapsible" : ""} ${isActive ? "active" : ""}" style="padding-left: ${12 + depth * 10}px;" ${hasHomeDocs ? `onclick="toggleNavCategory('${categoryId}')"` : ""}>${esc(key)}${hasHomeDocs ? ` <span class="collapse-icon ${isActive ? "active" : ""}">▶</span>` : ""}</div>`;
                if (hasHomeDocs) {
                    nodeHtml += `<div id="${categoryId}" class="collapsible-content ${isActive ? "active" : ""}">`;
                    const isOverviewActive = current === "Overview";
                    nodeHtml += `<a class="item ${isOverviewActive ? "active" : ""}" href="#Overview" onclick="selectModule('Overview');event.preventDefault();" style="padding-left: ${12 + (depth + 1) * 10}px;">OVERVIEW</a>`;
                    miscDocs.forEach(doc => {
                        const itemActive = current === doc.name;
                        nodeHtml += `<a class="item ${itemActive ? "active" : ""}" href="#${doc.name}" onclick="selectModule('${doc.name.replace(/'/g, "\\'")}', '${doc.name}');event.preventDefault();" style="padding-left: ${12 + (depth + 1) * 10}px;">${esc(doc.name)}</a>`;
                    });
                    nodeHtml += `</div>`;
                }
                if (depth === 0) {
                    nodeHtml += renderInfoSection(depth);
                }
                return;
            }

            const isDirectModule = currentCategory._modules.length === 1 && currentCategory._modules[0].name === newPath && Object.keys(currentCategory).filter(k => k !== "_modules").length === 0;

            if (isDirectModule) {
                const m = currentCategory._modules[0];
                const moduleId = `nav-mod-syms-${m.name.replace(/[^a-zA-Z0-9]/g, "_")}`;
                const hasSymbols = m.symbols && m.symbols.length > 0;
                const isModuleCurrent = current && (current === m.name || current.startsWith(`${m.name}::`));
                const moduleActive = isModuleCurrent;

                nodeHtml += `<div class="gtitle collapsible ${moduleActive ? "active" : ""}" style="padding-left: ${12 + depth * 10}px;" onclick="toggleNavModule('${moduleId}')"><span onclick="selectModule('${m.name.replace(/'/g, "\\\\'")}');event.stopPropagation();" style="cursor: pointer; flex: 1;">${esc(key)}</span><span class="collapse-icon ${moduleActive ? "active" : ""}">▶</span></div>`;
                nodeHtml += `<div id="${moduleId}" class="collapsible-content ${moduleActive ? "active" : ""}">`;
                m.symbols.sort((a, b) => a.name.localeCompare(b.name)).forEach(s => {
                    const isSymbolCurrent = current && current.includes("::") && current.split("::")[0] === m.name && (current.split("::")[1] === s.name || current.split("::")[1] === s.id);
                    const isFunction = s.kind === "function";
                    nodeHtml += `<a class="item ${isSymbolCurrent ? "active" : ""} ${isFunction ? "is-function" : ""}" href="#${m.name}::${s.id}" onclick="selectModule('${m.name.replace(/'/g, "\\'")}', '${s.id}');event.preventDefault();" style="padding-left: ${12 + (depth + 1) * 10}px;">${esc(s.name)}</a>`;
                });
                nodeHtml += `</div>`;
            } else {
                const hasChildren = currentCategory._modules.length > 0 || Object.keys(currentCategory).some(subKey => subKey !== "_modules");
                const isActive = isPathActive || (depth === 0);
                const categoryModule = currentCategory._modules.find(m => m.name === newPath);
                const navTarget = categoryModule ? categoryModule.name : newPath;

                nodeHtml += `<div class="gtitle ${hasChildren ? "collapsible" : ""} ${isActive ? "active" : ""}" style="padding-left: ${12 + depth * 10}px;" ${hasChildren ? `onclick="toggleNavCategory('${categoryId}')"` : ""}><span onclick="selectModule('${navTarget.replace(/'/g, "\\\\'")}');event.stopPropagation();" style="cursor: pointer; flex: 1;">${esc(key)}</span>${hasChildren ? `<span class="collapse-icon ${isActive ? "active" : ""}">▶</span>` : ""}</div>`;
                if (hasChildren) nodeHtml += `<div id="${categoryId}" class="collapsible-content ${isActive ? "active" : ""}">`;

                currentCategory._modules.sort((a, b) => a.name.localeCompare(b.name)).forEach(m => {
                    const moduleId = `nav-mod-syms-${m.name.replace(/[^a-zA-Z0-9]/g, "_")}`;
                    const hasSymbols = m.symbols && m.symbols.length > 0;
                    const isModuleCurrent = current && (current === m.name || current.startsWith(`${m.name}::`));
                    const moduleActive = isModuleCurrent;
                    const moduleShortName = m.name.split(".").pop();

                    if (hasSymbols) {
                        nodeHtml += `<div class="gtitle collapsible ${moduleActive ? "active" : ""}" style="padding-left: ${12 + (depth + 1) * 10}px;" onclick="toggleNavModule('${moduleId}')"><span onclick="selectModule('${m.name.replace(/'/g, "\\\\'")}');event.stopPropagation();" style="cursor: pointer; flex: 1;">${esc(moduleShortName)}</span><span class="collapse-icon ${moduleActive ? "active" : ""}">▶</span></div>`;
                        nodeHtml += `<div id="${moduleId}" class="collapsible-content ${moduleActive ? "active" : ""}">`;
                        m.symbols.sort((a, b) => a.name.localeCompare(b.name)).forEach(s => {
                            const isSymbolCurrent = current && current.includes("::") && current.split("::")[0] === m.name && (current.split("::")[1] === s.name || current.split("::")[1] === s.id);
                            const isFunction = s.kind === "function";
                            nodeHtml += `<a class="item ${isSymbolCurrent ? "active" : ""} ${isFunction ? "is-function" : ""}" href="#${m.name}::${s.id}" onclick="selectModule('${m.name.replace(/'/g, "\\'")}', '${s.id}');event.preventDefault();" style="padding-left: ${12 + (depth + 2) * 10}px;">${esc(s.name)}</a>`;
                        });
                        nodeHtml += `</div>`;
                    } else {
                        nodeHtml += `<a class="item${current === m.name ? " active" : ""}" href="#${m.name}" onclick="selectModule('${m.name.replace(/'/g, "\\'")}');event.preventDefault();" style="padding-left: ${12 + (depth + 1) * 10}px;">${esc(moduleShortName)}</a>`;
                    }
                });

                nodeHtml += renderTree(currentCategory, depth + 1, newPath);
                if (hasChildren) nodeHtml += `</div>`;
            }
        });
        return nodeHtml;
    }

    $("nav").innerHTML = renderTree(navTree, 0, "");
}

function header(title, desc, stats) {
    return `<div class="header"><div class="mod-label">SYS.REF</div><div class="title">${esc(title)}</div><div class="desc">${esc(desc || "Module interface reference.")}</div><div class="stats">${esc(stats)}</div></div>`;
}

function codeBox(code) {
    const s = String(code ?? "").trim();
    return `<div class="codebox"><div class="codehd"><span></span><button class="copy" onclick='copyText(${JSON.stringify(s).replace(/'/g, "&#39;")}, this); event.stopPropagation();'>COPY</button></div><pre><code>${highlight(s)}</code></pre></div>`;
}

function highlightNytrixCode(code) {
    if (!code) return "";
    return highlight(code);
}

function highlightAllCodeBlocks(selector) {
    document.querySelectorAll(`${selector} pre`).forEach(pre => {
        const codeEl = pre.querySelector("code");
        const raw = codeEl ? codeEl.textContent : pre.textContent;
        if (!raw.trim()) return;
        const className = ((codeEl ? codeEl.className : "") || "") + " " + (pre.className || "");
        const isNy = className.includes("language-ny") || className.includes("language-nytrix");
        const hasLanguage = className.includes("language-");
        if (hasLanguage && !isNy && typeof window.hljs !== "undefined" && codeEl) {
            window.hljs.highlightElement(codeEl);
            return;
        }
        const html = highlightNytrixCode(raw);
        if (codeEl) codeEl.innerHTML = html;
        else pre.innerHTML = `<code>${html}</code>`;
    });
}

function highlightExampleBlocks(selector) {
    document.querySelectorAll(`${selector} .example, ${selector} .smallexample`).forEach(block => {
        if (block.querySelector("code")) return;
        const raw = block.textContent;
        if (!raw.trim()) return;
        const html = highlightNytrixCode(raw);
        block.innerHTML = `<code>${html}</code>`;
    });
}

function highlightInlineCode(selector) {
    document.querySelectorAll(`${selector} code`).forEach(codeEl => {
        if (codeEl.closest("pre")) return;
        if (codeEl.querySelector(".syn-kw, .syn-str, .syn-num, .syn-call, .syn-com")) return;
        const raw = codeEl.textContent;
        if (!raw.trim()) return;
        if (!codeEl.classList.contains("code")) codeEl.classList.add("code");
        codeEl.innerHTML = highlightNytrixCode(raw);
    });
}

function renderOverviewPage(overviewMod, specificDocName = null) {
    const title = specificDocName || overviewMod.name;
    let html = header(title, overviewMod.module_doc, `Standard Library Documentation`, "");
    const docsToRender = specificDocName
        ? overviewMod.markdown_docs.filter(d => d.name === specificDocName)
        : overviewMod.markdown_docs.filter(d => d.name !== "NY" && d.name !== "NYTRIX");
    docsToRender.forEach(doc => {
        const body = doc.format === "html" ? doc.html : marked.parse(doc.html);
        const cls = doc.format === "html" ? "html-content" : "markdown-content";
        html += `<div class="md-section" id="md-${doc.name}"><div class="md-title">${esc(doc.name)}</div><div class="${cls}">${body}</div></div>`;
    });
    $("content").innerHTML = html;
    highlightAllCodeBlocks(".markdown-content");
    highlightAllCodeBlocks(".html-content");
    highlightExampleBlocks(".html-content");
    highlightInlineCode(".markdown-content");
    highlightInlineCode(".html-content");
    if (window.MathJax && window.MathJax.typesetPromise) window.MathJax.typesetPromise();
}

function renderModuleContent(mod) {
    const syms = (mod.symbols || []).slice().sort((a, b) => a.name.localeCompare(b.name));
    let html = header(mod.name, mod.module_doc, `${syms.length}_SYMS`, mod.orig_file);
    if (!syms.length) return html + `<div class="empty">NO_EXPORTS</div>`;
    html += `<div class="grid">`;
    syms.forEach(s => {
        const arg = getArgs(s.sig);
        html += `<div class="card" id="${s.id}" onclick="selectModule('${mod.name}', '${s.id}')"><div class="row"><div class="name"><span class="syn-module">${esc(mod.name)}</span> <span class="syn-call">${esc(s.name)}</span>${arg ? `(${esc(arg)})` : ""}</div><div class="kind">${esc(s.kind)}</div></div><div class="card-body"><div class="doc">${renderRichDocstring(s.doc)}</div>${s.imports && s.imports.length ? `<div class="imports"><div class="codehd"><span>IMPORTS</span></div>${(() => { const groups = {}; s.imports.forEach(imp => { const mod = imp.module_target || "global"; (groups[mod] || (groups[mod] = [])).push(imp); }); let html = "<ul>"; Object.keys(groups).sort().forEach(mod => { const symbolLinks = groups[mod].sort((a, b) => a.symbol_target.localeCompare(b.symbol_target)).map(imp => { const display = imp.alias ? `${imp.symbol_target} as ${imp.alias}` : imp.symbol_target; return `<a class="syn-call" href="#${imp.full_path}" onclick="selectModule('${imp.full_path}');event.preventDefault();">${esc(display)}</a>`; }).join(", "); html += mod === "global" ? `<li>${symbolLinks}</li>` : `<li><a class="syn-module" href="#${mod}" onclick="selectModule('${mod}');event.preventDefault();">${esc(mod)}</a>: ${symbolLinks}</li>`; }); html += "</ul>"; return html; })()}</div>` : ""}${s.code ? codeBox(s.code) : ""}</div></div>`;
    });
    html += `</div>`;
    return html;
}

function renderModule(name) {
    const mod = data.find(m => m.name === name);
    if (!mod) return;
    $("content").innerHTML = renderModuleContent(mod);
    if (window.MathJax && window.MathJax.typesetPromise) window.MathJax.typesetPromise();
}

function renderCategoryOverview(categoryPath) {
    const categoryModules = data.filter(m => m.name.startsWith(categoryPath + ".") || m.name === categoryPath);
    if (categoryModules.length === 0) { $("content").innerHTML = header(categoryPath, "Category Overview", "0 Modules") + `<div class="empty">NO_MODULES_FOUND</div>`; return; }
    categoryModules.sort((a, b) => a.name.localeCompare(b.name));
    let fullHtml = header(categoryPath, `Aggregated view of ${categoryModules.length} module(s) in this category.`);
    categoryModules.forEach(mod => { fullHtml += renderModuleContent(mod); });
    $("content").innerHTML = fullHtml;
    if (window.MathJax && window.MathJax.typesetPromise) window.MathJax.typesetPromise();
}

function selectModule(name, symbolNameOrId = null) {
    if (!symbolNameOrId && name.includes("::")) { const parts = name.split("::"); name = parts[0]; symbolNameOrId = parts[1]; }
    current = name;
    renderNav();
    let actualId = symbolNameOrId;

    const overviewMod = data.find(m => m.name === "Overview");
    const isMarkdown = overviewMod && overviewMod.markdown_docs.some(d => d.name === name);
    const isModule = data.some(m => m.name === name);
    const hasChildren = data.some(m => m.name.startsWith(name + "."));

    if (name === "Overview" || isMarkdown) {
        renderOverviewPage(overviewMod, isMarkdown ? name : null);
    } else if (hasChildren) {
        renderCategoryOverview(name);
    } else if (isModule) {
        renderModule(name);
        if (symbolNameOrId) {
            const mod = data.find(m => m.name === name);
            const symbol = mod && mod.symbols && mod.symbols.find(s => s.id === symbolNameOrId || s.name === symbolNameOrId);
            if (symbol) { actualId = symbol.id; current = `${name}:: ${symbol.name}`; }
        }
    }

    if (symbolNameOrId) {
        // setTimeout(() => { const symbolElement = $(actualId); if (symbolElement) symbolElement.scrollIntoView({ behavior: "smooth", block: "start" }); }, 50);
    } else $("content-area").scrollTo(0, 0);

    history.replaceState(null, null, "#" + name + (actualId ? `:: ${actualId}` : ""));

    const asideElement = $("aside");
    if (asideElement && asideElement.classList.contains("active")) toggleSidebar();
}

function doSearch() {
    const q = $("search").value.trim().toLowerCase();
    if (!q) { if (current) selectModule(current); else if (data.length) selectModule(data[0].name); return; }
    const hits = [];
    data.forEach(m => (m.symbols || []).forEach(s => { if ((s.name || "").toLowerCase().includes(q) || (s.doc || "").toLowerCase().includes(q)) hits.push({ mod: m.name, ...s }); }));
    let html = header("SEARCH", `QUERY: ${q}`, `${hits.length}_MATCHES`);
    if (!hits.length) { $("content").innerHTML = html + `<div class="empty">ERR_NO_MATCH</div>`; return; }
    html += `<div class="grid">`;
    hits.forEach(s => {
        const arg = getArgs(s.sig);
        html += `<div class="card"><div class="row"><div class="name"><span class="syn-module">${esc(s.mod)}</span> <span class="syn-call">${esc(s.name)}</span>${arg ? `(${esc(arg)})` : ""}</div><div class="kind">${esc(s.kind)}</div></div><div class="card-body"><div class="doc">${renderRichDocstring(s.doc)}</div>${s.imports && s.imports.length ? `<div class="imports"><div class="codehd"><span>IMPORTS</span></div>${(() => { const groups = {}; s.imports.forEach(imp => { const mod = imp.module_target || "global"; (groups[mod] || (groups[mod] = [])).push(imp); }); let html = "<ul>"; Object.keys(groups).sort().forEach(mod => { const symbolLinks = groups[mod].sort((a, b) => a.symbol_target.localeCompare(b.symbol_target)).map(imp => { const display = imp.alias ? `${imp.symbol_target} as ${imp.alias}` : imp.symbol_target; return `<a class="syn-call" href="#${imp.full_path}" onclick="selectModule('${imp.full_path}');event.preventDefault();">${esc(display)}</a>`; }).join(", "); html += mod === "global" ? `<li>${symbolLinks}</li>` : `<li><a class="syn-module" href="#${mod}" onclick="selectModule('${mod}');event.preventDefault();">${esc(mod)}</a>: ${symbolLinks}</li>`; }); html += "</ul>"; return html; })()}</div>` : ""}${s.code ? codeBox(s.code) : ""}</div></div>`;
    });
    $("content").innerHTML = html + `</div>`;
}

$("modcount").textContent = `${data.length}_MODS`;
$("symcount").textContent = `${countAllSyms()}_SYMS`;
const h = window.location.hash.slice(1);

document.addEventListener("keydown", e => {
    if (e.key === "Escape") {
        const aside = $("aside");
        if (aside && aside.classList.contains("active")) toggleSidebar();
        e.preventDefault();
        return;
    }
    if (e.target.tagName === "INPUT" || e.target.tagName === "TEXTAREA") return;
    if (e.ctrlKey || e.altKey || e.metaKey) return;
    if (e.key.length === 1) {
        const aside = $("aside");
        if (aside && !aside.classList.contains("active")) toggleSidebar();
        const searchInput = $("search");
        if (searchInput) searchInput.focus();
    }
});

if (h) {
    const overviewMod = data.find(m => m.name === "Overview");
    const isMarkdown = overviewMod && overviewMod.markdown_docs.some(d => d.name === h);
    const parts = h.split("::");
    if (parts.length > 1) selectModule(parts[0], parts[1]);
    else if (isMarkdown || data.find(m => m.name === h)) selectModule(h);
    else selectModule("Overview");
} else selectModule("Overview");
