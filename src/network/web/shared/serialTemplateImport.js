// Shared by desktop and mobile. SheetJS is loaded locally only for Excel files.
const MAX_ROWS = 1000;
const MAX_FILE_BYTES = 2 * 1024 * 1024;
let excelReaderPromise;

export function normalizeTemplateRows(rows) {
    const nonempty = rows.map((values, index) => ({ values, row: index + 1 }))
        .filter(({ values }) => values.some(value => String(value ?? '').trim()));
    if (!nonempty.length) throw new Error('表格为空');
    const header = nonempty.shift().values.map(value => String(value ?? '').replace(/^\uFEFF/, '').trim());
    const codeColumns = header.flatMap((value, index) => ['墙板编码', '命令码'].includes(value) ? [index] : []);
    const nameColumns = header.flatMap((value, index) => ['墙板名称', '名称'].includes(value) ? [index] : []);
    if (codeColumns.length !== 1 || nameColumns.length !== 1) {
        throw new Error('首行必须包含唯一的“墙板编码”和“墙板名称”列');
    }
    if (!nonempty.length || nonempty.length > MAX_ROWS) throw new Error('每次可导入 1～1000 条命令');
    const seen = new Map();
    let duplicateCount = 0;
    for (const { values, row } of nonempty) {
        const raw = String(values[codeColumns[0]] ?? '').trim();
        const data = raw.replace(/[\s:\-]/g, '').toUpperCase();
        const name = String(values[nameColumns[0]] ?? '').trim();
        if (!/^[0-9A-F]+$/.test(data) || data.length % 2 || data.length > 1024 || raw.length > 4096) {
            throw new Error(`第 ${row} 行：命令码必须是完整的十六进制字节（1～512 字节）`);
        }
        if (!name || new TextEncoder().encode(name).length > 512 || name.includes('\0')) {
            throw new Error(`第 ${row} 行：墙板名称不能为空，且不能超过 512 个 UTF-8 字节`);
        }
        if (seen.has(data)) {
            if (seen.get(data).name !== name) throw new Error(`第 ${row} 行：命令码 ${data} 对应多个名称`);
            duplicateCount++;
        } else {
            seen.set(data, { data, name });
        }
    }
    return { templates: [...seen.values()], duplicateCount };
}

export function parseTemplateCsv(text) {
    const rows = [];
    let row = [], field = '', quoted = false, closed = false;
    text = text.replace(/^\uFEFF/, '');
    for (let i = 0; i < text.length; i++) {
        const char = text[i];
        if (quoted) {
            if (char === '"' && text[i + 1] === '"') { field += '"'; i++; }
            else if (char === '"') { quoted = false; closed = true; }
            else field += char;
        } else if (char === '"' && !field && !closed) {
            quoted = true;
        } else if (char === ',' || char === '\n' || char === '\r') {
            row.push(field); field = ''; closed = false;
            if (char !== ',') {
                rows.push(row); row = [];
                if (char === '\r' && text[i + 1] === '\n') i++;
            }
        } else if (closed || char === '"') {
            throw new Error('CSV 引号格式错误，请从 Excel 重新另存为 CSV');
        } else field += char;
    }
    if (quoted) throw new Error('CSV 存在未闭合的引号');
    row.push(field); rows.push(row);
    return rows;
}

function loadExcelReader() {
    if (!excelReaderPromise) {
        excelReaderPromise = new Promise((resolve, reject) => {
            const script = document.createElement('script');
            script.src = new URL('../libs/xlsx.full.min.js', import.meta.url).href;
            script.onload = () => window.XLSX ? resolve(window.XLSX) : reject(new Error('Excel 读取组件不可用'));
            script.onerror = () => { script.remove(); reject(new Error('Excel 读取组件加载失败，请重试')); };
            document.head.appendChild(script);
        }).catch(error => { excelReaderPromise = null; throw error; });
    }
    return excelReaderPromise;
}

export async function readTemplateFile(file) {
    if (!file || file.size > MAX_FILE_BYTES) throw new Error('请选择不超过 2 MB 的表格');
    const extension = file.name.split('.').pop().toLowerCase();
    if (!['xlsx', 'xls', 'csv'].includes(extension)) throw new Error('请选择 .xlsx、.xls 或 .csv 表格');
    const bytes = new Uint8Array(await file.arrayBuffer());
    let rows;
    if (extension === 'csv') {
        let text;
        try { text = new TextDecoder('utf-8', { fatal: true }).decode(bytes); }
        catch (_) { text = new TextDecoder('gb18030', { fatal: true }).decode(bytes); }
        rows = parseTemplateCsv(text);
    } else {
        // Reject mislabeled text files instead of accepting SheetJS's text fallback.
        const xlsx = bytes[0] === 0x50 && bytes[1] === 0x4B;
        const xls = bytes[0] === 0xD0 && bytes[1] === 0xCF;
        if ((extension === 'xlsx' && !xlsx) || (extension === 'xls' && !xls)) {
            throw new Error('文件内容与扩展名不符，请用 Excel 重新保存');
        }
        const reader = await loadExcelReader();
        const workbook = reader.read(bytes, { type: 'array', sheetRows: MAX_ROWS + 2, cellFormula: true });
        const sheet = workbook.Sheets[workbook.SheetNames[0]];
        if (!sheet) throw new Error('Excel 没有可读取的工作表');
        for (const cell of Object.values(sheet)) {
            if (cell && typeof cell === 'object' && cell.f) throw new Error('导入表格不能包含公式，请先粘贴为值');
        }
        const range = reader.utils.decode_range(sheet['!fullref'] || sheet['!ref'] || 'A1');
        if (range.e.r >= MAX_ROWS + 1) throw new Error('每次可导入 1～1000 条命令');
        if (range.e.c > 31) throw new Error('表格最多支持 32 列');
        rows = reader.utils.sheet_to_json(sheet, { header: 1, defval: '', raw: false, blankrows: true });
    }
    return normalizeTemplateRows(rows);
}

export function initSerialTemplateImport({ getType, confirm, submit, refresh, log }) {
    const add = document.getElementById('serial-add-template-btn');
    if (!add || document.getElementById('serial-import-templates-btn')) return;
    const button = document.createElement('button');
    button.id = 'serial-import-templates-btn';
    button.className = 'btn small secondary';
    button.textContent = '导入表格';
    button.type = 'button';
    const sample = document.createElement('a');
    sample.className = 'btn small secondary';
    sample.textContent = '下载命令表';
    sample.href = new URL('../templates/rs485-wall-commands.xlsx', import.meta.url).href;
    sample.download = '485墙板命令.xlsx';
    const input = document.createElement('input');
    input.type = 'file'; input.accept = '.xlsx,.xls,.csv'; input.hidden = true;
    add.after(button, sample, input);
    let selectedType;
    button.addEventListener('click', () => { selectedType = getType(); input.click(); });
    input.addEventListener('change', async () => {
        const file = input.files?.[0];
        if (!file) return;
        button.disabled = true;
        try {
            const { templates, duplicateCount } = await readTemplateFile(file);
            if (getType() !== selectedType) throw new Error('串口类型已切换，请重新选择表格');
            const summary = templates.slice(0, 5).map(item => `${item.data}  ${item.name}`).join('\n');
            const message = `向 ${selectedType.toUpperCase()} 导入 ${templates.length} 条命令${duplicateCount ? `（合并 ${duplicateCount} 条重复行）` : ''}？\n${summary}${templates.length > 5 ? '\n…' : ''}\n相同命令码更新名称并保留已绑定功能；新命令需在工作区绑定功能。`;
            if (!await confirm(message, '导入墙板命令')) return;
            if (getType() !== selectedType) throw new Error('串口类型已切换，请重新选择表格');
            const result = await submit(selectedType, templates);
            if (!result) throw new Error('导入未确认成功，请检查命令日志并刷新列表核对后重试');
            log('导入墙板命令', 'success', `新增 ${result.created_count} 条，更新 ${result.updated_count} 条`);
            await refresh();
        } catch (error) {
            log('导入墙板命令失败', 'error', error.message || '表格读取失败');
        } finally {
            button.disabled = false;
            input.value = '';
        }
    });
}
