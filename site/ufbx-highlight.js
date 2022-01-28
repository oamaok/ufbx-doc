let globalContext = {
    prefix: "",
    locals: [],
    inReference: false,
}

const tokenTypes = {
    comment: "//[^\n]*|/\*.*?\\*/",
    name: "[A-Za-z_][A-Za-z_0-9]*",
    string: "\"(?:\\\"|[^\"])*?\"",
    line: "\n",
    space: "[ \\t\\r]+",
    op: "(?:->|<<|>>|.)",
}

const tokenNames = Object.keys(tokenTypes)
const tokenRegex = new RegExp(tokenNames.map(n => `(${tokenTypes[n]})`).join("|"), "g")

const keywords = new Set([
    "const",
    "if",
    "else",
    "for",
    "while",
    "return",
])

const builtins = new Set([
    "true",
    "false",
    "NULL",
])

const types = new Set([
    "int",
    "unsigned",
    "short",
    "long",
    "char",
    "void",
    "uint8_t",
    "int8_t",
    "uint16_t",
    "int16_t",
    "uint32_t",
    "int32_t",
    "uint64_t",
    "int64_t",
    "size_t",
    "ptrdiff_t",
    "float",
    "double",
])

function tokenize(source) {
    const re = new RegExp(tokenRegex)
    let match = null
    let tokens = [{ type: "line", text: "" }]
    while ((match = re.exec(source)) !== null) {
        for (let i = 0; i < tokenNames.length; i++) {
            const text = match[i + 1]
            if (text) {
                const type = tokenNames[i]
                tokens.push({ type, text })
                break
            }
        }
    }
    return tokens
}

function search(tokens, pattern) {
    const re = new RegExp(pattern, "dg")

    let parts = []
    let mapping = []
    let pos = 0, index = 0
    for (const token of tokens) {
        if (token.type === "comment" || token.type === "space") {
            index += 1
            continue
        }

        mapping[pos] = index
        let repr = ""
        if (token.type === "string" || token.type === "line") {
            repr = `${token.type} `
        } else {
            repr = `${token.type}:${token.text} `
        }
        parts.push(repr)
        for (let i = 0; i < repr.length; i++) {
            mapping[pos++] = index
        }
        index += 1
    }

    const str = parts.join("")

    let results = []
    let match = null
    while ((match = re.exec(str)) !== null) {
        const pos = mapping[match.index]
        results.push({
            pos: pos,
            token: tokens[pos],
            groups: (match.indices ?? []).map((pair) => {
                const begin = mapping[pair[0]]
                const end = mapping[pair[1]]
                return {
                    begin, end,
                    token: tokens[begin],
                    tokens: tokens.slice(begin, end),
                }
            })
        })
    }

    return results
}

function patchKeywords(tokens) {
    for (const m of search(tokens, /name/)) {
        if (keywords.has(m.token.text)) {
            m.token.type = "kw"
        } else if (builtins.has(m.token.text)) {
            m.token.type = "builtin"
        }
    }
}

function patchTypes(tokens) {
    for (const m of search(tokens, /name/)) {
        if (types.has(m.token.text)) {
            m.token.type = "type"
        } else if (m.token.text.startsWith("ufbx_")) {
            m.token.type = "type"
        }
    }
}

let globalDeclId = 0

function patchDecls(tokens) {
    for (const m of search(tokens, /line (?:kw:const )?(type:\S* )(?:op:\* )*(name:\S* )(?:op:= |op:; )/)) {
        const type = m.groups[1].token
        const name = m.groups[2].token
        name.declType = type.text
        name.declId = ++globalDeclId
    }
    for (const m of search(tokens, /(?:op:\( |op:, )(?:kw:const )?(type:\S* )(?:op:\* )*(name:\S* )(?:op:= |op:; )/)) {
        const type = m.groups[1].token
        const name = m.groups[2].token
        name.declType = type.text
        name.declId = ++globalDeclId
    }
}

function patchRefs(tokens) {
    let scopes = [{ }]
    let prevToken = { type: "line", text: "" }
    for (let i = 0; i < tokens.length; i++) {
        const token = tokens[i]
        if (token.text === "{") {
            scopes.push({ })
        } else if (token.text === "}") {
            scopes.pop()
        } else if (token.declType) {
            const scope = scopes[scopes.length - 1]
            scope[token.text] = { type: token.declType, id: token.declId }
        } else if (token.type === "name" && prevToken.text !== ".") {
            for (let i = scopes.length - 1; i >= 0; i--) {
                const scope = scopes[i]
                const ref = scope[token.text]
                if (ref) {
                    token.refType = ref.type
                    token.refId = ref.id
                }
            }
        }

        prevToken = token
    }
}

function patchFields(tokens) {
    for (const m of search(tokens, /(name:\S* )(?:op:. |op:-> )(name:\S* )/)) {
        const parent = m.groups[1].token
        const field = m.groups[2].token
        if (parent.refType) {
            field.structType = parent.refType
        }
    }
    for (const m of search(tokens, /(type:\S* )op:. (name:\S* )/)) {
        const parent = m.groups[1].token
        const field = m.groups[2].token
        if (parent.refType) {
            field.structType = parent.text
        }
    }
}

function patchInitFields(tokens) {
    for (const m of search(tokens, /line op:. (name:\S* )/)) {
        const name = m.groups[1].token
        let pos = m.groups[1].begin
        let numOpen = 0
        for (; pos >= 0; pos--) {
            const tok = tokens[pos]
            if (tok.text === "{") {
                numOpen += 1
            } else if (tok.type === "line" && numOpen > 0) {
                break
            } else if (tok.declType) {
                name.structType = tok.declType
                break
            }
        }
    }
}

function linkRef(ref) {
    if (globalContext.inReference) {
        return `#${ref}`
    } else {
        return `/reference#${ref}`
    }
}

function highlight(str) {
    const tokens = tokenize(str)
    patchKeywords(tokens)
    patchTypes(tokens)
    patchDecls(tokens)
    patchRefs(tokens)
    patchFields(tokens)
    patchInitFields(tokens)

    return tokens.map((token) => {
        if (token.type === "op" || token.type === "line" || token.type === "space") {
            return token.text
        } else if (token.type === "comment") {
            return `<span class="c-comment">${token.text}</span>`
        } else {
            let classes = [`c-${token.type}`]
            let tag = "span"
            let attribs = { }
            if (token.declId) attribs["data-decl-id"] = token.declId
            if (token.refId) attribs["data-ref-id"] = token.refId
            attribs["class"] = classes.join(" ")

            if (token.text.startsWith("ufbx_")) {
                tag = "a"
                attribs["href"] = `/reference#${token.text}`
            } else if (token.structType) {
                tag = "a"
                attribs["href"] = `/reference#${token.structType}.${token.text}`
            }

            const attribStr = Object.keys(attribs).map(key => `${key}="${attribs[key]}"`).join(" ")
            return `<${tag} ${attribStr}>${token.text}</${tag}>`
        }
    }).join("")

    if (false) {
        str = str.replace(/<((?:ufbx_|UFBX_)[A-Za-z0-9_\.]+)>.([A-Za-z0-9_]+)[A-Za-z0-9_/]*/g, (sub, root, field) => {
            const href = linkRef(`${root}.${field}`.toLowerCase())
            return `.<a class="dl" href="${href}">${field}</a>`
        })
        str = str.replace(/(?<!#)((ufbx_|UFBX_)[A-Za-z0-9_\.]+)[A-Za-z0-9_/]*/g, (sub, root) => {
            const href = linkRef(root.toLowerCase())
            return `<a class="dl" href="${href}">${sub}</a>`
        })
        for (const local of globalContext.locals) {
            str = str.replace(new RegExp(`\\b${local}\\b`, "g"), (sub) => {
                const href = linkRef(globalContext.prefix + local)
                return `<a class="ll" href="${href}">${sub}</a>`
            })
        }
        str = str.replace(keywordRegex, (sub) => {
            return `<span class="kw">${sub}</span>`
        })
        str = str.replace(builtinRegex, (sub) => {
            return `<span class="bt">${sub}</span>`
        })
    }
    return str
}

function setHighlightContext(ctx) {
    const prev = globalContext
    globalContext = ctx
    return prev
}

module.exports = {
    highlight,
    setHighlightContext,
}
