// Generates the site's specification pages from docs/rig-spec.md, the
// language's single source of truth. Nothing in site/spec/ is written by
// hand: this script splits the specification into one page per top-level
// section, rewrites its links for the site and turns its examples into
// labelled blocks. `npm run docs:build` and `npm run docs:dev` run it first,
// and the dev server runs it again whenever the specification changes.
//
// What it writes (all git-ignored):
//   site/spec/index.md                  the preamble (title, status, contents)
//   site/spec/<section>.md              one page per section and appendix
//   site/.vitepress/generated/sidebar.json   the sidebar, read by config.mts
//   site/.vitepress/generated/taste.md       section 1's example, for the home page
//
// Any link whose target does not exist (a section anchor, a "section N.M"
// reference or a repository file) fails the generation, so a broken link in
// the specification fails the site's build.

import { execFileSync } from 'node:child_process'
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath, pathToFileURL } from 'node:url'

const here = path.dirname(fileURLToPath(import.meta.url))
export const siteDir = path.resolve(here, '..')
export const repoDir = path.resolve(siteDir, '..')
export const specPath = path.join(repoDir, 'docs', 'rig-spec.md')
const outDir = path.join(siteDir, 'spec')
const generatedDir = path.join(siteDir, '.vitepress', 'generated')

export const repoUrl = 'https://github.com/WoozChucky/rig'
const specDirInRepo = 'docs'

// Repository paths the site never links to: the original design documents.
// A link of the specification into one keeps its words and loses its link.
const unlinked = ['docs/superpowers']

// The labels of a `rig` block's kinds (spec section 1.4, "Examples").
const kindLabels = {
  error: 'Compile error',
  'runtime-error': 'Runtime error',
  fragment: 'Fragment'
}

class SpecError extends Error {}

// GitHub's heading anchors: lower case, punctuation other than '-' and '_'
// removed, spaces turned into '-', and a repeated anchor numbered '-1', '-2'.
// The site keeps GitHub's anchors so that a link into docs/rig-spec.md on
// GitHub and one into the site name a heading the same way.
function githubSlug(text) {
  return text
    .trim()
    .toLowerCase()
    .replace(/[^\p{L}\p{M}\p{N}\p{Pc} -]/gu, '')
    .replace(/ /g, '-')
}

// A heading's plain text: inline code, emphasis and links reduced to their text.
function plainText(markdown) {
  return markdown
    .replace(/!?\[([^\]]*)\]\([^)]*\)/g, '$1')
    .replace(/`+([^`]*)`+/g, '$1')
    .replace(/[*_]{1,3}([^*_]+)[*_]{1,3}/g, '$1')
}

// --- Reading the specification -------------------------------------------

// Splits the specification into lines, finds its code blocks (each line
// marked as inside one or not) and its headings. Every `## ` heading after the preamble starts
// a page: `## N. Title` or `## Appendix X: Title`.
function parse(source) {
  const lines = source.replace(/\r\n?/g, '\n').split('\n')
  const headings = []
  const fences = [] // { start, end, lang, info }: lines [start, end), the fence lines included
  const used = new Map()
  const inFence = []
  let fence = null

  lines.forEach((line, i) => {
    const marker = /^(`{3,}|~{3,})\s*(.*)$/.exec(line)
    if (fence) {
      inFence.push(true)
      if (marker && marker[1][0] === fence.marker[0] && marker[1].length >= fence.marker.length && !marker[2]) {
        fences.push({ start: fence.start, end: i + 1, lang: fence.lang, info: fence.info })
        fence = null
      }
      return
    }
    if (marker) {
      const [, lang = '', info = ''] = /^(\S*)\s*(.*)$/.exec(marker[2])
      fence = { marker: marker[1], start: i, lang, info: info.trim() }
      inFence.push(true)
      return
    }
    if (/^\s+(`{3,}|~{3,})/.test(line)) {
      throw new SpecError(`docs/rig-spec.md:${i + 1}: an indented code fence; the site only handles fences at the margin`)
    }
    inFence.push(false)
    const h = /^(#{1,6})\s+(.*?)\s*#*\s*$/.exec(line)
    if (!h) return
    const text = plainText(h[2])
    let id = githubSlug(text)
    const count = used.get(id) ?? 0
    used.set(id, count + 1)
    if (count > 0) id = `${id}-${count}`
    headings.push({ line: i, level: h[1].length, markdown: h[2], text, id })
  })
  if (fence) throw new SpecError(`docs/rig-spec.md:${fence.start + 1}: a code block is never closed`)

  // The pages. The preamble is everything before the first section.
  const pages = []
  const preamble = { slug: 'index', title: null, number: null, start: 0, headings: [] }
  pages.push(preamble)
  for (const h of headings) {
    if (h.level === 1 && pages.length === 1) preamble.title = h.text
    if (h.level === 2) {
      const numbered = /^(\d+)\.\s+(.*)$/.exec(h.text)
      const appendix = /^Appendix ([A-Z]):\s+(.*)$/.exec(h.text)
      if (numbered || appendix) {
        const [, number, title] = numbered ?? appendix
        const slug = githubSlug(title)
        if (pages.some((p) => p.slug === slug)) {
          throw new SpecError(`docs/rig-spec.md:${h.line + 1}: two sections would both be the page spec/${slug}`)
        }
        pages.push({ slug, title, number, appendix: !!appendix, start: h.line, heading: h, headings: [] })
      } else if (pages.length > 1) {
        throw new SpecError(
          `docs/rig-spec.md:${h.line + 1}: '## ${h.text}' is neither '## N. Title' nor '## Appendix X: Title'`
        )
      }
    }
    pages[pages.length - 1].headings.push(h)
  }
  if (pages.length === 1) throw new SpecError('docs/rig-spec.md has no "## 1. Title" sections')
  pages.forEach((p, i) => (p.end = i + 1 < pages.length ? pages[i + 1].start : lines.length))

  // Where every anchor lives, and what "section N.M" names.
  const anchors = new Map()
  const sections = new Map()
  for (const page of pages) {
    for (const h of page.headings) {
      anchors.set(h.id, { page, heading: h })
      const n = /^(\d+(?:\.\d+)*|[A-Z](?:\.\d+)+)\.?\s/.exec(h.text)
      if (n && page !== preamble) sections.set(n[1], { page, heading: h })
    }
    if (page.appendix) sections.set(page.number, { page, heading: page.heading })
  }
  const headingAt = new Map(headings.map((h) => [h.line, h]))
  return { lines, inFence, fences, headingAt, pages, anchors, sections }
}

// --- Links ----------------------------------------------------------------

function pageUrl(page) {
  return page.slug === 'index' ? '/spec/' : `/spec/${page.slug}`
}

function anchorUrl(target) {
  // A page's own title links to the page itself.
  if (target.heading === target.page.heading || target.heading.level === 1) return pageUrl(target.page)
  return `${pageUrl(target.page)}#${target.heading.id}`
}

// Resolves a link of the specification to the site: an anchor to its page,
// a repository file to GitHub, anything with a scheme left alone. Returns
// null for a link that is to be shown as its words alone (`unlinked`).
function resolveLink(spec, target, where, errors, image = false) {
  if (/^[a-z][a-z0-9+.-]*:/i.test(target) || target.startsWith('//')) return target
  const hashAt = target.indexOf('#')
  const file = hashAt < 0 ? target : target.slice(0, hashAt)
  const hash = hashAt < 0 ? '' : target.slice(hashAt + 1)

  const repoPath = file === '' ? null : path.posix.normalize(path.posix.join(specDirInRepo, decodeURI(file)))
  if (file === '' || repoPath === `${specDirInRepo}/rig-spec.md`) {
    if (hash === '') return '/spec/'
    const found = spec.anchors.get(decodeURIComponent(hash))
    if (!found) {
      errors.push(`${where}: the link '${target}' names no heading of the specification`)
      return target
    }
    return anchorUrl(found)
  }
  if (repoPath.startsWith('..')) {
    errors.push(`${where}: the link '${target}' leaves the repository`)
    return target
  }
  const onDisk = path.join(repoDir, ...repoPath.split('/'))
  if (!fs.existsSync(onDisk)) {
    errors.push(`${where}: the link '${target}' names ${repoPath}, which does not exist`)
    return target
  }
  if (unlinked.some((dir) => repoPath === dir || repoPath.startsWith(`${dir}/`))) return null
  const kind = image ? 'raw' : fs.statSync(onDisk).isDirectory() ? 'tree' : 'blob'
  return `${repoUrl}/${kind}/main/${encodeURI(repoPath)}${hash ? `#${hash}` : ''}`
}

// The site's link to section `number` ("5.10", "8", "A").
function sectionUrl(spec, number, where, errors) {
  const found = spec.sections.get(number)
  if (!found) {
    errors.push(`${where}: section ${number} is referred to but does not exist`)
    return null
  }
  return anchorUrl(found)
}

// Splits a line of prose into inline code spans, links and plain text, so
// that links are rewritten and plain text gets its section references linked
// without touching code.
const inlineRE = /(`+)[^`][\s\S]*?\1(?!`)|!?\[((?:[^[\]\\]|\\.|\[[^\]]*\])*)\]\(\s*<?([^)\s>]*)>?(\s+"[^"]*")?\s*\)/g

function rewriteInline(spec, text, where, errors, { linkSections }) {
  let out = ''
  let last = 0
  for (const m of text.matchAll(inlineRE)) {
    out += plain(text.slice(last, m.index))
    if (m[1]) {
      out += m[0]
    } else {
      const image = m[0].startsWith('!')
      const url = resolveLink(spec, m[3], where, errors, image)
      out += url === null ? (image ? '' : m[2]) : `${image ? '!' : ''}[${m[2]}](${url}${m[4] ?? ''})`
    }
    last = m.index + m[0].length
  }
  return out + plain(text.slice(last))

  function plain(s) {
    if (!linkSections) return s
    // "section 5.10", "sections 6.2, 6.4", "sections 3 to 7", "Appendix A".
    s = s.replace(
      /\b(sections?)(\s+)(\d+(?:\.\d+)*(?:(?:,\s*|\s+and\s+|\s+or\s+|\s+to\s+)\d+(?:\.\d+)*)*)/gi,
      (all, word, space, list) => {
        const numbers = list.match(/\d+(?:\.\d+)*/g)
        if (numbers.length === 1) {
          const url = sectionUrl(spec, numbers[0], where, errors)
          return url ? `[${all}](${url})` : all
        }
        return (
          word +
          space +
          list.replace(/\d+(?:\.\d+)*/g, (n) => {
            const url = sectionUrl(spec, n, where, errors)
            return url ? `[${n}](${url})` : n
          })
        )
      }
    )
    return s.replace(/\bAppendix ([A-Z])\b/g, (all, letter) => {
      const url = sectionUrl(spec, letter, where, errors)
      return url ? `[${all}](${url})` : all
    })
  }
}

// A table whose last column is "Section" (Appendix B) gets its numbers linked.
function rewriteSectionColumn(spec, line, where, errors) {
  const cells = line.split(/(?<!\\)\|/)
  // cells: '', c1, ..., cN, '' for a line '| c1 | ... | cN |'
  const lastIndex = cells.length - 2
  cells[lastIndex] = cells[lastIndex].replace(/\b(\d+(?:\.\d+)*)\b/g, (n) => {
    const url = sectionUrl(spec, n, where, errors)
    return url ? `[${n}](${url})` : n
  })
  return cells.join('|')
}

// --- Examples ---------------------------------------------------------------

// Turns a `rig` block of a kind into the markup of a labelled example, and
// its output, the first `text` block after it before the next heading or
// `rig` block (spec section 1.4), into an "Output" block, attached to the
// example when nothing but blank lines lies between them. A `rig
// file=NAME.rig` block, a file the next example loads, is labelled with its
// name.
function exampleLabel(kind, where, errors) {
  if (kind === '') return null
  if (kindLabels[kind]) return { cls: kind, text: kindLabels[kind] }
  const exit = /^exit=(\d+)$/.exec(kind)
  if (exit) return { cls: 'exit', text: `Exits with status ${exit[1]}` }
  // Another file of the next example's program, which it #loads. Its name
  // is held to the test runner's rule (tests/runner.cpp's
  // is_example_file_name), so it needs no escaping in the label: `file=` as
  // written, ASCII letters, digits, '_', '-' and '.', ending in .rig in any
  // case, and neither example.rig nor a Windows device's (NUL.rig), in any
  // case.
  const file = /^file=([A-Za-z0-9_.-]+\.[Rr][Ii][Gg])$/.exec(kind)
  if (file) {
    const stem = file[1].split('.')[0]
    if (!/^example\.rig$/i.test(file[1]) && !/^(con|prn|aux|nul|com\d|lpt\d)$/i.test(stem)) {
      return { cls: 'file', text: `File ${file[1]}` }
    }
  }
  errors.push(`${where}: unknown kind of rig block '${kind}'`)
  return null
}

function exampleHtml(label, code, output) {
  const parts = [`<div class="rig-example${label ? ` rig-example-${label.cls}` : ''}">`]
  if (label) parts.push(`<div class="rig-example-label">${label.text}</div>`)
  parts.push('', ...code, '')
  if (output) parts.push('<div class="rig-example-output">Output</div>', '', ...output, '')
  parts.push('</div>')
  return parts
}

// The fenced blocks within lines [start, end).
function blocks(spec, start, end) {
  return spec.fences.filter((b) => b.start >= start && b.end <= end)
}

// --- Pages ------------------------------------------------------------------

function renderPage(spec, page, lastUpdated, errors) {
  const out = []
  const fenced = blocks(spec, page.start, page.end)
  const byStart = new Map(fenced.map((b) => [b.start, b]))
  const isHeading = (i) => !spec.inFence[i] && /^#{1,6}\s/.test(spec.lines[i])

  // Pair each rig block with its output. A file= block is no example, so it
  // has none, as the test runner takes it.
  const outputs = new Set() // the starts of text blocks that are outputs
  const exampleOf = new Map() // rig block start -> output block
  for (const [k, b] of fenced.entries()) {
    if (b.lang !== 'rig' || /(^|\s)file=/.test(b.info)) continue
    for (let n = k + 1; n < fenced.length; n++) {
      const next = fenced[n]
      if (next.lang === 'rig') break
      let headingBetween = false
      for (let i = b.end; i < next.start; i++) if (isHeading(i)) headingBetween = true
      if (headingBetween) break
      if (next.lang === 'text') {
        let attached = true
        for (let i = b.end; i < next.start; i++) if (spec.lines[i].trim() !== '') attached = false
        outputs.add(next.start)
        if (attached) exampleOf.set(b.start, next)
        break
      }
    }
  }

  let table = null // { sectionColumn } while inside a table
  let i = page.start
  while (i < page.end) {
    const line = spec.lines[i]
    const where = `docs/rig-spec.md:${i + 1}`
    const block = byStart.get(i)
    if (block) {
      table = null
      const body = spec.lines.slice(block.start + 1, block.end - 1)
      const fence = (lang) => ['```' + lang, ...body, '```']
      if (block.lang === 'rig') {
        const label = exampleLabel(block.info, where, errors)
        const output = exampleOf.get(block.start)
        if (label || output) {
          const outBody = output && spec.lines.slice(output.start + 1, output.end - 1)
          out.push(...exampleHtml(label, fence('rig'), output && ['```text', ...outBody, '```']))
          i = output ? output.end : block.end
          continue
        }
        out.push(...fence('rig'))
      } else if (block.lang === 'text' && outputs.has(block.start)) {
        out.push('<div class="rig-example rig-example-detached">', '<div class="rig-example-output">Output</div>')
        out.push('', ...fence('text'), '', '</div>')
      } else {
        out.push(...spec.lines.slice(block.start, block.end))
      }
      i = block.end
      continue
    }

    const h = !spec.inFence[i] && /^(#{1,6})\s+/.exec(line)
    if (h) {
      table = null
      const heading = spec.headingAt.get(i)
      // A section's `## N. Title` is its page's title; every heading under it moves up a level.
      const level = page.slug === 'index' ? h[1].length : h[1].length - 1
      out.push(`${'#'.repeat(level)} ${heading.markdown} {#${heading.id}}`)
      i++
      continue
    }

    if (/^\s*\|/.test(line)) {
      if (!table) {
        const cells = line.split(/(?<!\\)\|/).map((c) => c.trim())
        table = { sectionColumn: /^sections?$/i.test(cells[cells.length - 2] ?? '') }
        out.push(rewriteInline(spec, line, where, errors, { linkSections: true }))
      } else if (/^\s*\|[\s:|-]+\|\s*$/.test(line)) {
        out.push(line)
      } else {
        let row = rewriteInline(spec, line, where, errors, { linkSections: true })
        if (table.sectionColumn) row = rewriteSectionColumn(spec, row, where, errors)
        out.push(row)
      }
      i++
      continue
    }
    table = null

    // Reference-style link definitions.
    const def = /^(\s{0,3}\[[^\]]+\]:\s*)(\S+)(.*)$/.exec(line)
    if (def) {
      const url = resolveLink(spec, def[2], where, errors)
      if (url !== null) out.push(def[1] + url + def[3])
    } else {
      out.push(rewriteInline(spec, line, where, errors, { linkSections: true }))
    }
    i++
  }

  const front = ['---']
  if (page.slug === 'index') front.push('title: Specification')
  else front.push(`title: ${JSON.stringify(page.heading.text)}`)
  if (lastUpdated) front.push(`lastUpdated: ${lastUpdated}`)
  front.push('---', '')
  return front.join('\n') + '\n' + out.join('\n').trim() + '\n'
}

// The sidebar: the contents, then every section with its subsections.
function sidebar(spec) {
  const items = [{ text: 'Contents', link: '/spec/' }]
  for (const page of spec.pages.slice(1)) {
    const subsections = page.headings
      .filter((h) => h.level === 3)
      .map((h) => ({ text: h.text, link: `${pageUrl(page)}#${h.id}` }))
    const item = { text: page.heading.text, link: pageUrl(page) }
    if (subsections.length) Object.assign(item, { collapsed: true, items: subsections })
    items.push(item)
  }
  return [{ text: 'Specification', items }]
}

// Section 1's first complete program and its output, for the home page,
// with a line saying where in the specification it comes from.
function taste(spec, errors) {
  const intro = spec.pages[1]
  const fenced = blocks(spec, intro.start, intro.end)
  const k = fenced.findIndex((b) => b.lang === 'rig' && b.info === '')
  if (k < 0) {
    errors.push(`docs/rig-spec.md: section ${intro.number} has no complete program for the home page`)
    return ''
  }
  const program = fenced[k]
  // Its output: the first `text` block after it, before the next heading or `rig` block.
  const next = fenced.slice(k + 1).find((b) => b.lang === 'text' || b.lang === 'rig')
  const headingBefore = (line) => intro.headings.filter((h) => h.line < line).at(-1)
  const output =
    next && next.lang === 'text' && headingBefore(next.start) === headingBefore(program.start) ? next : null
  const body = (b) => spec.lines.slice(b.start + 1, b.end - 1)
  const lines = exampleHtml(
    null,
    ['```rig', ...body(program), '```'],
    output ? ['```text', ...body(output), '```'] : null
  )
  const from = headingBefore(program.start)
  lines.push('', `From [${from.text}](${anchorUrl({ page: intro, heading: from })}) of the specification.`)
  return lines.join('\n') + '\n'
}

// The home page's links into the specification, checked as the
// specification's own are: a page or an anchor that is gone fails the build.
function checkHomeLinks(spec, errors) {
  const home = fs.readFileSync(path.join(siteDir, 'index.md'), 'utf8')
  for (const m of home.matchAll(/\/spec\/([\w-]*)(?:#([\w-]+))?/g)) {
    const page = spec.pages.find((p) => p.slug === (m[1] || 'index'))
    if (!page) errors.push(`site/index.md: the link '${m[0]}' names no page of the specification`)
    else if (m[2] && !page.headings.some((h) => h.id === m[2])) {
      errors.push(`site/index.md: the link '${m[0]}' names no heading of that page`)
    }
  }
}

// The specification's last commit's date, for the pages' "Last updated".
function lastCommitDate() {
  try {
    const out = execFileSync('git', ['log', '-1', '--format=%cI', '--', 'docs/rig-spec.md'], {
      cwd: repoDir,
      encoding: 'utf8',
      stdio: ['ignore', 'pipe', 'ignore']
    }).trim()
    return out || null
  } catch {
    return null
  }
}

function writeIfChanged(file, content) {
  if (fs.existsSync(file) && fs.readFileSync(file, 'utf8') === content) return false
  fs.mkdirSync(path.dirname(file), { recursive: true })
  fs.writeFileSync(file, content)
  return true
}

// Generates every page. Returns { sidebarChanged }, or throws a SpecError
// listing every broken link.
export function generate() {
  const spec = parse(fs.readFileSync(specPath, 'utf8'))

  const errors = []
  const lastUpdated = lastCommitDate()
  const files = new Map()
  for (const page of spec.pages) {
    files.set(path.join(outDir, `${page.slug}.md`), renderPage(spec, page, lastUpdated, errors))
  }
  const tasteMd = taste(spec, errors)
  checkHomeLinks(spec, errors)
  if (errors.length) {
    throw new SpecError(`${errors.length} broken link(s) in the specification:\n  ${errors.join('\n  ')}`)
  }

  for (const [file, content] of files) writeIfChanged(file, content)
  // Pages of sections that no longer exist.
  if (fs.existsSync(outDir)) {
    for (const name of fs.readdirSync(outDir)) {
      const file = path.join(outDir, name)
      if (!files.has(file)) fs.rmSync(file, { recursive: true, force: true })
    }
  }
  writeIfChanged(path.join(generatedDir, 'taste.md'), tasteMd)
  const sidebarChanged = writeIfChanged(
    path.join(generatedDir, 'sidebar.json'),
    JSON.stringify(sidebar(spec), null, 2) + '\n'
  )
  return { pages: files.size, sidebarChanged }
}

// `node scripts/spec.mjs`: generate once.
if (process.argv[1] && import.meta.url === pathToFileURL(path.resolve(process.argv[1])).href) {
  try {
    const { pages } = generate()
    console.log(`spec: ${pages} pages generated from docs/rig-spec.md`)
  } catch (e) {
    console.error(`spec: ${e.message}`)
    process.exit(1)
  }
}
