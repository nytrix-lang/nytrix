let current = null;
const $ = id => document.getElementById(id);
const esc = s => String(s ?? '').replace(/[&<>"']/g, m => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[m]));
const countAllSyms = () => data.reduce((n, m) => n + (m.symbols ? m.symbols.length : 0), 0);

function highlight(code) {
    if (!code) return '';
    const rules = [
        { name: 'str', regex: /(?:f?"(?:\\.|[^"])*"|f?'(?:\\.|[^'])*')/ },
        { name: 'com', regex: /(?:;[^\n]*|\/\/.*)/ },
        { name: 'kw', regex: /\b(?:fn|def|if|else|elif|while|for|return|use|import|export|module|case|break|continue|asm|const|let|struct|enum|match|type|ptr|null|true|false)\b/ },
        { name: 'num', regex: /\b(?:0x[0-9a-fA-F]+|[0-9]+(?:\.[0-9]+)?)\b/ },
        { name: 'call', regex: /\b(?:[a-zA-Z_][a-zA-Z0-9_]*)(?=\s*\()/ }
    ];
    const full = new RegExp(rules.map(r => '(' + r.regex.source + ')').join('|'), 'g');
    let lastIdx = 0, out = '', m;
    while ((m = full.exec(code)) !== null) {
        out += esc(code.substring(lastIdx, m.index));
        for (let i = 0; i < rules.length; i++) {
            if (m[i + 1] !== undefined) {
                out += `<span class="syn-${rules[i].name}">${esc(m[i + 1])}</span>`;
                break;
            }
        }
        lastIdx = full.lastIndex;
    }
    return out + esc(code.substring(lastIdx));
}

function renderRichDocstring(text) {
    if (!text) return '';

    // Escape HTML first
    let html = esc(text);

    // Convert markdown-like syntax
    html = html
        .replace(/\*\*([^*]+)\*\*/g, '<b>$1</b>')
        .replace(/\*([^*]+)\*/g, '<i>$1</i>')
        .replace(/`([^`]+)`/g, '<code>$1</code>')
        // Convert simple lists
        .replace(/^\s*[-*]\s+(.*)$/gm, '<ul><li>$1</li></ul>')
        // Collapse adjacent <ul> tags
        .replace(/<\/ul>\n<ul>/g, '\n')
        // Convert [[module::symbol]] to link
        .replace(/\[\[([a-zA-Z0-9_.:(\), ]+)\]\]/g, (match, p1) => {
            const parts = p1.split('::');
            if (parts.length > 1) {
                return `<a href="#${p1}" onclick="selectModule('${parts[0]}', '${parts[1]}');event.preventDefault();">${esc(p1)}</a>`;
            }
            return `<a href="#${p1}" onclick="selectModule('${p1}');event.preventDefault();">${esc(p1)}</a>`;
        });

    return html;
}

const getArgs = sig => (sig || '').match(/\((.*)\)/)?.[1]?.trim() || '';

function copyText(t, btn) {
    navigator.clipboard.writeText(String(t ?? '')).then(() => {
        const old = btn.textContent; btn.textContent = 'OK';
        setTimeout(() => { btn.textContent = old; }, 1000);
    });
}

function toggleNavCategory(categoryId) {
    const categoryElement = $(
        categoryId);
    const gtitleElement = categoryElement.previousElementSibling; // The gtitle is the previous sibling

    if (categoryElement.classList.contains('active')) {
        categoryElement.classList.remove('active');
        gtitleElement.classList.remove('active');
    } else {
        categoryElement.classList.add('active');
        gtitleElement.classList.add('active');
    }
}

function toggleNavModule(moduleId) {
    const moduleElement = $(
        moduleId);
    const gtitleElement = moduleElement.previousElementSibling; // The gtitle is the previous sibling

    if (moduleElement.classList.contains('active')) {
        moduleElement.classList.remove('active');
        gtitleElement.classList.remove('active');
    } else {
        moduleElement.classList.add('active');
        gtitleElement.classList.add('active');
    }
}

function toggleOverviewModule(moduleId) {
    const moduleElement = $(
        moduleId);
    const cardRowElement = moduleElement.previousElementSibling; // The row inside the card is the previous sibling

    if (moduleElement.classList.contains('active')) {
        moduleElement.classList.remove('active');
        cardRowElement.classList.remove('active');
    } else {
        moduleElement.classList.add('active');
        cardRowElement.classList.add('active');
    }
}



function toggleSidebar() {
    const asideElement = $('aside');
    const mainElement = $('content-area');
    const backdrop = $('backdrop');
    const hamburger = $('hamburger');
    if (asideElement && mainElement) {
        const isActive = asideElement.classList.toggle('active');
        mainElement.classList.toggle('aside-active');
        if (backdrop) backdrop.classList.toggle('active', isActive);
        if (hamburger) hamburger.classList.toggle('active', isActive);

        // Clear search input when closing sidebar
        if (!isActive) {
            const searchInput = $('search');
            if (searchInput) {
                searchInput.value = '';
            }
        }
    }
}



function renderNav() {
    const navTree = {}; // Will hold the nested structure
    data.forEach(m => {
        let currentNode = navTree;
        m.path.forEach((component, index) => {
            if (!currentNode[component]) {
                currentNode[component] = { _modules: [] }; // Use _modules to store actual modules at this level
            }
            if (index === m.path.length - 1) { // Last component, add module here
                currentNode[component]._modules.push(m);
            }
            currentNode = currentNode[component];
        });
    });

    const overviewMod = data.find(m => m.name === "Overview");
    let mdDocs = overviewMod ? overviewMod.markdown_docs : [];

    // Sort Markdown docs by priority
    mdDocs.sort((docA, docB) => {
        const priority = ['README', 'LANGUAGE', 'METAPROGRAMMING', 'PROPOSALS', 'DOCKER', 'TODO'];
        const a = docA.name;
        const b = docB.name;

        const ia = priority.indexOf(a);
        const ib = priority.indexOf(b);

        if (ia !== -1 && ib !== -1) return ia - ib;
        if (ia !== -1) return -1;
        if (ib !== -1) return 1;

        return a.localeCompare(b);
    });

    let html = '';

    // Render the tree recursively
    function renderTree(node, depth = 0, currentPath = '') { // Added currentPath parameter
        let nodeHtml = '';
        // Sort current level keys (categories)
        const sortedKeys = Object.keys(node).sort((a, b) => {
            // Priority list for sidebar sections (matches filename case from docs/)
            const priority = ['Home', 'README', 'LANGUAGE', 'METAPROGRAMMING', 'PROPOSALS', 'DOCKER', 'TODO'];

            const ia = priority.indexOf(a);
            const ib = priority.indexOf(b);

            if (ia !== -1 && ib !== -1) return ia - ib;
            if (ia !== -1) return -1;
            if (ib !== -1) return 1;

            // Default alpha sort for everything else
            return a.localeCompare(b);
        });

        sortedKeys.forEach(key => {
            if (key === '_modules') return; // Skip the module storage key

            const newPath = currentPath ? `${currentPath}.${key}` : key;
            const currentCategory = node[key];
            const categoryId = `nav-cat-${newPath.replace(/[^a-zA-Z0-9]/g, '_')}-${depth}`;
            const isPathActive = current && (current === newPath || current.startsWith(newPath + '.') || current.startsWith(newPath + '::'));

            if (key === "Home") {
                const isActive = (depth === 0) || (current === "Overview") || (overviewMod && overviewMod.markdown_docs.some(d => d.name === current));
                nodeHtml += `<div class="gtitle ${mdDocs.length ? 'collapsible' : ''} ${isActive ? 'active' : ''}" style="padding-left: ${12 + depth * 10}px;" ${mdDocs.length ? `onclick="toggleNavCategory('${categoryId}')"` : ''}>
                                        ${esc(key)}
                                        ${mdDocs.length ? `<span class="collapse-icon ${isActive ? 'active' : ''}">▶</span>` : ''}
                                     </div>`;
                if (mdDocs.length) {
                    nodeHtml += `<div id="${categoryId}" class="collapsible-content ${isActive ? 'active' : ''}">`;

                    const isOverviewActive = current === "Overview";
                    nodeHtml += `<a class="item ${isOverviewActive ? 'active' : ''}" href="#Overview" onclick="selectModule('Overview');event.preventDefault();" style="padding-left: ${12 + (depth + 1) * 10}px;">
                                            OVERVIEW
                                         </a>`;

                    mdDocs.forEach(doc => {
                        const itemActive = current === doc.name;
                        nodeHtml += `<a class="item ${itemActive ? 'active' : ''}" href="#${doc.name}" onclick="selectModule('${doc.name.replace(/'/g, "\'")}', '${doc.name}');event.preventDefault();" style="padding-left: ${12 + (depth + 1) * 10}px;">
                                                ${esc(doc.name)}
                                             </a>`;
                    });
                    nodeHtml += `</div>`;
                }
                return;
            }

            // Check if this 'key' represents a module that should be rendered directly
            const isDirectModule = currentCategory._modules.length === 1 && currentCategory._modules[0].name === newPath && Object.keys(currentCategory).filter(k => k !== '_modules').length === 0;

            if (isDirectModule) {
                const m = currentCategory._modules[0];
                const moduleId = `nav-mod-syms-${m.name.replace(/[^a-zA-Z0-9]/g, '_')}`; // Use full module name for ID
                const hasSymbols = m.symbols && m.symbols.length > 0;
                const isModuleCurrent = current && (current === m.name || current.startsWith(`${m.name}::`));
                const moduleActive = (isModuleCurrent);

                nodeHtml += `<div class="gtitle collapsible ${moduleActive ? 'active' : ''}" style="padding-left: ${12 + depth * 10}px;" onclick="toggleNavModule('${moduleId}')">
                                        <span onclick="selectModule('${m.name.replace(/'/g, "\\'")}');event.stopPropagation();" style="cursor: pointer; flex: 1;">${esc(key)}</span>
                                        <span class="collapse-icon ${moduleActive ? 'active' : ''}">▶</span>
                                     </div>`;
                nodeHtml += `<div id="${moduleId}" class="collapsible-content ${moduleActive ? 'active' : ''}">`;
                m.symbols.sort((a, b) => a.name.localeCompare(b.name)).forEach(s => {
                    const isSymbolCurrent = current && current.includes('::') && current.split('::')[0] === m.name && (current.split('::')[1] === s.name || current.split('::')[1] === s.id);
                    const isFunction = s.kind === 'function';
                    nodeHtml += `<a class="item ${isSymbolCurrent ? 'active' : ''} ${isFunction ? 'is-function' : ''}" href="#${m.name}::${s.id}" onclick="selectModule('${m.name.replace(/'/g, "\'")}', '${s.id}');event.preventDefault();" style="padding-left: ${12 + (depth + 1) * 10}px;">
                                            ${esc(s.name)}
                                         </a>`;
                });
                nodeHtml += `</div>`; // Close collapsible-content

            } else { // Regular category or category with multiple modules/subcategories
                const hasChildren = currentCategory._modules.length > 0 || Object.keys(currentCategory).some(subKey => subKey !== '_modules');

                const isActive = isPathActive || (depth === 0);
                // Check if there's a module with the same name as the category for navigation
                const categoryModule = currentCategory._modules.find(m => m.name === newPath);
                const navTarget = categoryModule ? categoryModule.name : newPath;

                nodeHtml += `<div class="gtitle ${hasChildren ? 'collapsible' : ''} ${isActive ? 'active' : ''}" style="padding-left: ${12 + depth * 10}px;" ${hasChildren ? `onclick="toggleNavCategory('${categoryId}')"` : ''}>
                                        <span onclick="selectModule('${navTarget.replace(/'/g, "\\'")}');event.stopPropagation();" style="cursor: pointer; flex: 1;">${esc(key)}</span>
                                        ${hasChildren ? `<span class="collapse-icon ${isActive ? 'active' : ''}">▶</span>` : ''}
                                     </div>`;

                if (hasChildren) {
                    nodeHtml += `<div id="${categoryId}" class="collapsible-content ${isActive ? 'active' : ''}">`;
                }

                // Render actual modules under this category (if any)
                currentCategory._modules.sort((a, b) => a.name.localeCompare(b.name)).forEach(m => {
                    const moduleId = `nav-mod-syms-${m.name.replace(/[^a-zA-Z0-9]/g, '_')}`;
                    const hasSymbols = m.symbols && m.symbols.length > 0;
                    const isModuleCurrent = current && (current === m.name || current.startsWith(`${m.name}::`));
                    const moduleActive = (isModuleCurrent);
                    const moduleShortName = m.name.split('.').pop(); // Get last component of module name

                    if (hasSymbols) {
                        nodeHtml += `<div class="gtitle collapsible ${moduleActive ? 'active' : ''}" style="padding-left: ${12 + (depth + 1) * 10}px;" onclick="toggleNavModule('${moduleId}')">
                                                <span onclick="selectModule('${m.name.replace(/'/g, "\\'")}');event.stopPropagation();" style="cursor: pointer; flex: 1;">${esc(moduleShortName)}</span>
                                                <span class="collapse-icon ${moduleActive ? 'active' : ''}">▶</span>
                                             </div>`;
                        nodeHtml += `<div id="${moduleId}" class="collapsible-content ${moduleActive ? 'active' : ''}">`;
                        m.symbols.sort((a, b) => a.name.localeCompare(b.name)).forEach(s => {
                            const isSymbolCurrent = current && current.includes('::') && current.split('::')[0] === m.name && (current.split('::')[1] === s.name || current.split('::')[1] === s.id);
                            const isFunction = s.kind === 'function';
                            nodeHtml += `<a class="item ${isSymbolCurrent ? 'active' : ''} ${isFunction ? 'is-function' : ''}" href="#${m.name}::${s.id}" onclick="selectModule('${m.name.replace(/'/g, "\'")}', '${s.id}');event.preventDefault();" style="padding-left: ${12 + (depth + 2) * 10}px;">
                                                    ${esc(s.name)}
                                                 </a>`;
                        });
                        nodeHtml += `</div>`; // Close collapsible-content
                    } else {
                        // If no symbols, render as a simple item
                        nodeHtml += `<a class="item${current === m.name ? ' active' : ''}" href="#${m.name}" onclick="selectModule('${m.name.replace(/'/g, "\'")}');event.preventDefault();" style="padding-left: ${12 + (depth + 1) * 10}px;">
                                                ${esc(moduleShortName)}
                                             </a>`;
                    }
                });

                // Recurse for subcategories
                nodeHtml += renderTree(currentCategory, depth + 1, newPath); // Pass newPath

                if (hasChildren) {
                    nodeHtml += `</div>`; // Close collapsible-content
                }
            }
        });
        return nodeHtml;
    }

    html += renderTree(navTree, 0, ''); // Initial call with empty path
    $(
        'nav').innerHTML = html;
}

function header(title, desc, stats) {
    return `<div class="header"><div class="mod-label">SYS.REF</div><div class="title">${esc(title)}</div><div class="desc">${esc(desc || 'Module interface reference.')}</div><div class="stats">${esc(stats)}</div></div>`;
}

function codeBox(code) {
    const s = String(code ?? '').trim();
    return `<div class="codebox"><div class="codehd"><span></span><button class="copy" onclick='copyText(${JSON.stringify(s).replace(/'/g, "&#39;")}, this)'>COPY</button></div><pre><code>${highlight(s)}</code></pre></div>`;
}

// Syntax highlight Nytrix code
function highlightNytrixCode(code) {
    if (!code) return '';
    // Don't highlight if it looks like shell commands or other non-Nytrix code
    if (code.includes('sudo ') || code.includes('xdg-open') || code.includes('#!/') || code.includes('make ')) {
        return esc(code);
    }
    return highlight(code);
}

function renderOverviewPage(overviewMod, specificDocName = null) {
    const title = specificDocName || overviewMod.name;
    let html = header(title, overviewMod.module_doc, `Standard Library Documentation`, "");

    const docsToRender = specificDocName
        ? overviewMod.markdown_docs.filter(d => d.name === specificDocName)
        : overviewMod.markdown_docs;

    docsToRender.forEach(doc => {
        html += `<div class="md-section" id="md-${doc.name}">
                            <div class="md-title">${esc(doc.name)}</div>
                            <div class="markdown-content">${marked.parse(doc.html)}</div>
                         </div>`;
    });

    $('content').innerHTML = html;

    // Apply syntax highlighting only to Nytrix code blocks
    document.querySelectorAll('.markdown-content pre code').forEach(block => {
        const className = block.className || '';
        // Only highlight if explicitly marked as Nytrix or if no language specified and looks like Nytrix
        if (className.includes('language-ny') || className.includes('language-nytrix') ||
            (!className.includes('language-') && (block.textContent.includes('fn ') || block.textContent.includes('def ')))) {
            const code = block.textContent;
            block.innerHTML = highlightNytrixCode(code);
        }
    });

    if (window.MathJax && window.MathJax.typesetPromise) {
        window.MathJax.typesetPromise();
    }
}

function renderModuleContent(mod) {
    const syms = (mod.symbols || []).slice().sort((a, b) => a.name.localeCompare(b.name));
    let html = header(mod.name, mod.module_doc, `${syms.length}_SYMS`, mod.orig_file);
    if (!syms.length) {
        return html + '<div class="empty">NO_EXPORTS</div>';
    }
    html += `<div class="grid">`;
    syms.forEach(s => {
        const arg = getArgs(s.sig);
        const full = arg ? `${mod.name}::${s.name}(${arg})` : `${mod.name}::${s.name}`;
        html += `<div class="card" id="${s.id}">
                    <div class="row">
                        <div class="name">
                            <span class="syn-module">${esc(mod.name)}</span> <span class="syn-call">${esc(s.name)}</span>${arg ? `(${esc(arg)})` : ''}
                        </div>
                        <div class="kind">${esc(s.kind)}</div>
                    </div>
                    <div class="card-body">
                        <div class="doc">${renderRichDocstring(s.doc)}</div>
                        ${s.imports && s.imports.length ? `
                            <div class="imports">
                                <div class="codehd"><span>IMPORTS</span></div>
                                ${(() => {
                    const groups = {};
                    s.imports.forEach(imp => {
                        const mod = imp.module_target || 'global';
                        if (!groups[mod]) groups[mod] = [];
                        groups[mod].push(imp);
                    });

                    let html = '<ul>';
                    Object.keys(groups).sort().forEach(mod => {
                        const symbolLinks = groups[mod].sort((a, b) => a.symbol_target.localeCompare(b.symbol_target)).map(imp => {
                            const display = imp.alias ? `${imp.symbol_target} as ${imp.alias}` : imp.symbol_target;
                            return `<a class="syn-call" href="#${imp.full_path}" onclick="selectModule('${imp.full_path}');event.preventDefault();">${esc(display)}</a>`;
                        }).join(', ');

                        if (mod === 'global') {
                            html += `<li>${symbolLinks}</li>`;
                        } else {
                            html += `<li><a class="syn-module" href="#${mod}" onclick="selectModule('${mod}');event.preventDefault();">${esc(mod)}</a>: ${symbolLinks}</li>`;
                        }
                    });
                    html += '</ul>';
                    return html;
                })()}
                            </div>
                        ` : ''}
                        ${s.code ? codeBox(s.code) : ''}
                    </div>
                </div>`;
    });
    html += `</div>`;
    return html;
}

function renderModule(name) {
    const mod = data.find(m => m.name === name);
    if (!mod) return;
    $('content').innerHTML = renderModuleContent(mod);
    if (window.MathJax && window.MathJax.typesetPromise) {
        window.MathJax.typesetPromise();
    }
}

function renderCategoryOverview(categoryPath) {
    const categoryModules = data.filter(m => m.name.startsWith(categoryPath + '.') || m.name === categoryPath);

    if (categoryModules.length === 0) {
        $('content').innerHTML = header(categoryPath, 'Category Overview', '0 Modules') + '<div class="empty">NO_MODULES_FOUND</div>';
        return;
    }

    categoryModules.sort((a, b) => a.name.localeCompare(b.name));

    let fullHtml = header(categoryPath, `Aggregated view of ${categoryModules.length} module(s) in this category.`);

    categoryModules.forEach(mod => {
        fullHtml += renderModuleContent(mod);
    });

    $('content').innerHTML = fullHtml;
    if (window.MathJax && window.MathJax.typesetPromise) {
        window.MathJax.typesetPromise();
    }
}

function selectModule(name, symbolNameOrId = null) {
    if (!symbolNameOrId && name.includes('::')) {
        const parts = name.split('::');
        name = parts[0];
        symbolNameOrId = parts[1];
    }
    current = name;
    renderNav();
    let actualId = symbolNameOrId;

    const overviewMod = data.find(m => m.name === "Overview");
    const isMarkdown = overviewMod && overviewMod.markdown_docs.some(d => d.name === name);
    const isModule = data.some(m => m.name === name);
    const hasChildren = data.some(m => m.name.startsWith(name + '.'));

    if (name === "Overview" || isMarkdown) {
        renderOverviewPage(overviewMod, isMarkdown ? name : null);
    } else if (hasChildren) {
        renderCategoryOverview(name);
    } else if (isModule) {
        renderModule(name);
        if (symbolNameOrId) {
            const mod = data.find(m => m.name === name);
            const symbol = mod && mod.symbols && mod.symbols.find(s => s.id === symbolNameOrId || s.name === symbolNameOrId);
            if (symbol) {
                actualId = symbol.id;
                current = `${name}:: ${symbol.name}`;
            }
        }
    }
    // $('meta').textContent = name.toUpperCase();

    if (symbolNameOrId) {
        setTimeout(() => {
            const symbolElement = $(actualId);
            if (symbolElement) {
                symbolElement.scrollIntoView({ behavior: 'smooth', block: 'start' });
            }
        }, 50);
    } else {
        $('content-area').scrollTo(0, 0);
    }

    history.replaceState(null, null, '#' + name + (actualId ? `:: ${actualId}` : ''));

    // Close sidebar on click
    const asideElement = $('aside');
    if (asideElement && asideElement.classList.contains('active')) {
        toggleSidebar();
    }
}

function doSearch() {
    const q = $(
        'search').value.trim().toLowerCase();
    if (!q) { if (current) selectModule(current); else if (data.length) selectModule(data[0].name); return; }
    const hits = [];
    data.forEach(m => (m.symbols || []).forEach(s => { if ((s.name || '').toLowerCase().includes(q) || (s.doc || '').toLowerCase().includes(q)) hits.push({ mod: m.name, ...s }); }));
    let html = header('SEARCH', `QUERY: ${q}`, `${hits.length}_MATCHES`);
    if (!hits.length) {
        $(
            'content').innerHTML = html + '<div class="empty">ERR_NO_MATCH</div>'; return;
    }
    html += `<div class="grid">`;
    hits.forEach(s => {
        const arg = getArgs(s.sig);
        const full = arg ? `${s.mod}::${s.name}(${arg})` : `${s.mod}::${s.name}`;
        html += `<div class="card">
                    <div class="row">
                        <div class="name">
                            <span class="syn-module">${esc(s.mod)}</span> <span class="syn-call">${esc(s.name)}</span>${arg ? `(${esc(arg)})` : ''}
                        </div>
                        <div class="kind">${esc(s.kind)}</div>
                    </div>
                    <div class="card-body">
                        <div class="doc">${renderRichDocstring(s.doc)}</div>
                        ${s.imports && s.imports.length ? `
                            <div class="imports">
                                <div class="codehd"><span>IMPORTS</span></div>
                                ${(() => {
                    const groups = {};
                    s.imports.forEach(imp => {
                        const mod = imp.module_target || 'global';
                        if (!groups[mod]) groups[mod] = [];
                        groups[mod].push(imp);
                    });

                    let html = '<ul>';
                    Object.keys(groups).sort().forEach(mod => {
                        const symbolLinks = groups[mod].sort((a, b) => a.symbol_target.localeCompare(b.symbol_target)).map(imp => {
                            const display = imp.alias ? `${imp.symbol_target} as ${imp.alias}` : imp.symbol_target;
                            return `<a class="syn-call" href="#${imp.full_path}" onclick="selectModule('${imp.full_path}');event.preventDefault();">${esc(display)}</a>`;
                        }).join(', ');

                        if (mod === 'global') {
                            html += `<li>${symbolLinks}</li>`;
                        } else {
                            html += `<li><a class="syn-module" href="#${mod}" onclick="selectModule('${mod}');event.preventDefault();">${esc(mod)}</a>: ${symbolLinks}</li>`;
                        }
                    });
                    html += '</ul>';
                    return html;
                })()}
                            </div>
                        ` : ''}
                        ${s.code ? codeBox(s.code) : ''}
                    </div>
                </div>`;
    });
    $(
        'content').innerHTML = html + `</div>`;
}

$(
    'modcount').textContent = `${data.length}_MODS`;
$(
    'symcount').textContent = `${countAllSyms()}_SYMS`;
const h = window.location.hash.slice(1);

// Global hotkey for search
document.addEventListener('keydown', (e) => {
    // Always allow ESC to close the sidebar
    if (e.key === 'Escape') {
        const aside = $('aside');
        if (aside && aside.classList.contains('active')) {
            toggleSidebar();
        }
        // Always return after handling Escape to prevent further default actions
        // such as clearing search input if it was focused.
        e.preventDefault(); // Prevent default browser behavior (e.g., closing popups)
        return;
    }

    // Ignore if user is already typing in an input for other key presses
    if (e.target.tagName === 'INPUT' || e.target.tagName === 'TEXTAREA') {
        // If an input is focused, and it's not Escape, we do nothing more.
        return;
    }

    // Ignore control keys, combos for non-input areas
    if (e.ctrlKey || e.altKey || e.metaKey) return;

    // Check for printable characters (roughly) when not in an input
    if (e.key.length === 1) {
        // Open sidebar if not active
        const aside = $('aside');
        if (aside && !aside.classList.contains('active')) {
            toggleSidebar();
        }

        const searchInput = $('search');
        if (searchInput) {
            searchInput.focus();
            // We do NOT prevent default, so the char gets typed into the now-focused input
        }
    }
});
if (h) {
    const overviewMod = data.find(m => m.name === "Overview");
    const isMarkdown = overviewMod && overviewMod.markdown_docs.some(d => d.name === h);
    const parts = h.split('::');

    if (parts.length > 1) {
        selectModule(parts[0], parts[1]);
    } else if (isMarkdown || data.find(m => m.name === h)) {
        selectModule(h);
    } else {
        selectModule("Overview");
    }
} else { // Default to Overview page
    selectModule("Overview");
}
