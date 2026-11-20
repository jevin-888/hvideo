// Regenerate with: node scripts/generate_lyric_pinyin.mjs
// Reads the repository's existing pinyin-pro bundle; no network or npm install.
import { readFileSync, writeFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { createHash } from 'node:crypto';
import vm from 'node:vm';

const sourceUrl = new URL('../src/network/web/libs/pinyin-pro.js', import.meta.url);
const outputUrl = new URL('../src/lyric/LyricPinyinData.inc', import.meta.url);
const source = readFileSync(sourceUrl, 'utf8');
const marker = 'Object.defineProperty(n,"__esModule",';
if (source.split(marker).length !== 2) {
  throw new Error('pinyin-pro bundle changed: review the dictionary extraction marker.');
}
const exports = {};
vm.runInNewContext(source.replace(marker,
  'n.__lyricData={characters:y,phrases:[m,S,O,C,A]};' + marker),
  { exports, module: { exports } }, { timeout: 10000 });
const data = exports.__lyricData;
if (!data || Object.keys(data.characters).length < 3000 || data.phrases.length !== 5) {
  throw new Error('pinyin-pro dictionaries no longer match the expected structure.');
}

const isHan = cp => cp === 0x3007 || (cp >= 0x3400 && cp <= 0x4dbf) ||
  (cp >= 0x4e00 && cp <= 0x9fff) || (cp >= 0xf900 && cp <= 0xfaff) ||
  (cp >= 0x20000 && cp <= 0x2fa1f) || (cp >= 0x30000 && cp <= 0x323af);
const characters = new Map();
for (const chars of Object.values(data.characters)) {
  for (const char of chars) {
    const cp = char.codePointAt(0);
    if ([...char].length !== 1) throw new Error('Expected one Unicode code point.');
    if (isHan(cp)) {
      const readings = exports.pinyin(char, { type: 'array', toneType: 'symbol' });
      if (readings.length !== 1 || readings[0] === char) throw new Error(`No default reading for ${char}`);
      characters.set(cp, readings[0]);
    }
  }
}
const phrases = new Map();
for (const dictionary of data.phrases) {
  for (const [phrase, reading] of Object.entries(dictionary)) {
    if ([...phrase].every(char => isHan(char.codePointAt(0)))) phrases.set(phrase, reading);
  }
}

// Small, explicit corrections/context additions for common lyric vocabulary.
// The bundled general-purpose dictionary lacks 快乐 and reads 行长 as háng cháng.
for (const [phrase, reading] of Object.entries({
  '快乐': 'kuài lè', '行长': 'háng zhǎng', '长长': 'cháng cháng',
  '慢慢地': 'màn màn de', '轻轻地': 'qīng qīng de', '悄悄地': 'qiāo qiāo de',
  '静静地': 'jìng jìng de', '深深地': 'shēn shēn de', '渐渐地': 'jiàn jiàn de',
  '默默地': 'mò mò de', '紧紧地': 'jǐn jǐn de', '轻轻': 'qīng qīng',
})) phrases.set(phrase, reading);

// Common traditional variants are normalized only for phrase lookup. Character
// fallback always uses the original code point, retaining its own pronunciation.
// This intentionally does not claim complete simplified/traditional conversion.
const variantPairs = [
  '樂乐', '長长', '銀银', '慶庆', '為为', '覺觉', '著着', '發发', '髮发',
  '還还', '說说', '別别', '難难', '間间', '傳传', '彈弹', '調调', '數数',
  '轉转', '種种', '幹干', '乾干', '後后', '裡里', '裏里', '幾几', '參参',
  '應应', '愛爱', '見见', '聽听', '夢梦', '華华', '葉叶', '單单', '興兴',
  '復复', '複复', '經经', '場场', '處处', '盡尽', '儘尽', '寧宁', '創创',
  '強强', '遠远', '隱隐', '當当', '惡恶', '輕轻', '靜静', '漸渐', '緊紧',
  '風风', '雲云', '時时', '無无', '國国', '會会', '機机', '過过',
  '對对', '開开', '關关', '門门', '頭头', '來来', '從从', '進进', '動动',
  '讓让', '誰谁', '這这', '個个', '們们', '麼么', '嗎吗', '問问', '請请',
  '語语', '話话', '認认', '記记', '憶忆', '該该', '讀读', '書书', '寫写',
  '電电', '車车', '馬马', '飛飞', '鳥鸟', '魚鱼', '鄉乡', '樓楼', '橋桥',
  '園园', '圓圆', '圍围', '連连', '邊边', '點点', '線线', '實实',
  '現现', '樣样', '聲声', '結结', '終终', '總总', '給给', '紅红',
  '綠绿', '藍蓝', '黃黄', '萬万', '億亿', '兩两', '雙双', '歲岁',
  '爾尔', '兒儿', '淚泪', '滿满', '運运', '達达', '選选', '遙遥',
  '傾倾', '傷伤', '體体', '倆俩', '側侧', '價价', '優优', '偉伟',
  '倉仓', '涼凉', '凍冻', '劃划', '劍剑', '勞劳', '勸劝', '勢势',
  '區区', '協协', '卻却', '歷历', '厲厉', '壓压', '厭厌', '嘆叹', '嘗尝',
  '嘩哗', '聖圣', '堅坚', '墊垫', '牆墙', '壞坏', '壘垒', '壯壮',
  '奮奋', '奪夺', '奧奥', '婁娄', '孫孙', '學学', '寶宝', '審审', '寬宽',
  '將将', '層层', '屬属', '島岛', '嶺岭', '廣广', '廠厂', '彎弯', '彙汇',
  '徑径', '態态', '慣惯', '慘惨', '憐怜', '憤愤', '戀恋', '懷怀', '懸悬',
  '戰战', '戲戏', '擁拥', '擇择', '擊击', '擋挡', '擔担', '據据', '揚扬',
  '搖摇', '搶抢', '換换', '揮挥', '擾扰', '撫抚', '擺摆', '敗败', '斷断',
  '於于', '暈晕', '曉晓', '樹树', '標标', '橫横', '櫻樱',
  '檔档', '檢检', '權权', '歡欢', '歸归', '殘残', '殼壳', '氣气', '漢汉',
  '湯汤', '溫温', '滄沧', '滾滚', '澤泽', '濃浓', '濟济', '濤涛', '灣湾',
  '燈灯', '燒烧', '爭争', '爺爷', '牽牵', '獨独', '獲获', '獻献', '環环',
  '瑣琐', '畢毕', '畫画', '異异', '瘋疯', '癡痴', '皺皱', '監监', '蓋盖',
  '盤盘', '眾众', '睜睁', '睏困', '礦矿', '禮礼', '禍祸', '離离', '稅税',
  '穩稳', '窮穷', '簾帘', '籠笼', '籤签', '糧粮', '紛纷', '紙纸', '純纯',
  '細细', '組组', '絕绝', '統统', '絲丝', '綿绵', '維维', '緣缘', '緩缓',
  '編编', '練练', '縣县', '縱纵', '織织', '繞绕', '繼继', '續续', '纏缠',
  '罷罢', '羅罗', '義义', '習习', '翹翘', '聯联', '職职', '腦脑', '腳脚',
  '脫脱', '臉脸', '臟脏', '臺台', '舉举', '艱艰', '節节', '蘇苏', '藥药',
  '蘭兰', '虛虚', '蟲虫', '蠻蛮', '補补', '裝装', '裊袅', '襯衬', '觸触',
  '觀观', '親亲', '訪访', '計计', '訂订', '討讨', '託托', '訴诉', '試试',
  '詩诗', '誠诚', '誤误', '諸诸', '謎谜', '謝谢', '謠谣', '證证', '識识',
  '譯译', '議议', '護护', '變变', '豐丰', '貝贝', '貞贞', '負负', '財财',
  '貢贡', '貧贫', '貨货', '責责', '貴贵', '貫贯', '費费', '貼贴', '賤贱',
  '賜赐', '賠赔', '賢贤', '賣卖', '賺赚', '購购', '贈赠', '贊赞', '贏赢',
  '趙赵', '趕赶', '躍跃', '軌轨', '軍军', '軟软', '輩辈', '輝辉', '輪轮',
  '輸输', '辦办', '辯辩', '農农', '適适', '遲迟', '遺遗', '鄧邓', '郵邮',
  '釋释', '針针', '釣钓', '鈴铃', '銘铭', '鋼钢', '錯错', '錢钱', '錦锦',
  '錶表', '鍋锅', '鍵键', '鍾钟', '鐘钟', '鏡镜', '鐵铁', '鑽钻', '閃闪',
  '閉闭', '閒闲', '閑闲', '閱阅', '闖闯', '階阶', '陣阵', '陽阳', '陰阴',
  '陳陈', '陸陆', '隨随', '險险', '隻只', '雜杂', '雞鸡', '雖虽', '靈灵',
  '響响', '頁页', '頂顶', '順顺', '頑顽', '領领', '頓顿', '願愿', '顏颜',
  '顛颠', '類类', '顯显', '飄飘', '餅饼', '飯饭', '飲饮', '餓饿', '饒饶',
  '餘余', '館馆', '駐驻', '駕驾', '騎骑', '驚惊', '驗验', '骯肮', '鬧闹',
  '鬱郁', '魯鲁', '鮮鲜', '鴻鸿', '鷹鹰', '鹽盐', '麗丽', '麥麦', '齊齐',
  '齒齿', '龍龙', '龜龟',
];
const variants = new Map();
for (const pair of variantPairs) {
  const [traditional, simplified, extra] = [...pair];
  if (extra || !simplified) throw new Error(`Invalid variant pair: ${pair}`);
  if (traditional !== simplified) variants.set(traditional.codePointAt(0), simplified.codePointAt(0));
}

const syllables = [...new Set(characters.values())].sort();
const syllableIndices = new Map(syllables.map((syllable, index) => [syllable, index]));
if (syllables.length > 65535) throw new Error('Syllable indices exceed uint16_t.');
const phraseEntries = [...phrases].sort(([a], [b]) => Buffer.compare(Buffer.from(a), Buffer.from(b)));
for (const [phrase, reading] of phraseEntries) {
  if ([...phrase].length !== reading.split(' ').length) {
    throw new Error(`Phrase does not have one syllable per code point: ${phrase}`);
  }
}
const license = `MIT License

Copyright (c) 2022-present zh-lx

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.`;
const quote = value => JSON.stringify(value);
const hex = value => `0x${value.toString(16)}`;
const lines = [
  '// Generated by scripts/generate_lyric_pinyin.mjs. Do not edit by hand.',
  '// Dictionary: https://github.com/zh-lx/pinyin-pro (bundled web/libs/pinyin-pro.js).',
  `// Source SHA-256: ${createHash('sha256').update(source).digest('hex')}`,
  `// ${characters.size} Han characters, ${phrases.size} phrases, ${variants.size} traditional aliases.`,
  '/*', license, '*/', '',
  'static constexpr const char* kPinyinSyllables[] = {',
  ...syllables.map(syllable => `    ${quote(syllable)},`), '};', '',
  'static constexpr CharacterReading kCharacterReadings[] = {',
  ...[...characters].sort(([a], [b]) => a - b).map(([cp, syllable]) =>
    `    {${hex(cp)}, ${syllableIndices.get(syllable)}},`), '};', '',
  'static constexpr PhraseReading kPhraseReadings[] = {',
  ...phraseEntries.map(([phrase, reading]) => `    {${quote(phrase)}, ${quote(reading)}},`), '};', '',
  `static constexpr size_t kMaxPhraseCodepoints = ${Math.max(...phraseEntries.map(([phrase]) => [...phrase].length))};`,
  'static constexpr CharacterVariant kTraditionalVariants[] = {',
  ...[...variants].sort(([a], [b]) => a - b).map(([from, to]) => `    {${hex(from)}, ${hex(to)}},`), '};', '',
];
writeFileSync(outputUrl, lines.join('\n'), 'utf8');
console.log(`Generated ${fileURLToPath(outputUrl)}: ${characters.size} characters, ${phrases.size} phrases, ${variants.size} aliases.`);
