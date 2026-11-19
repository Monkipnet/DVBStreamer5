'use strict';
const assert=require('node:assert/strict');
const fs=require('node:fs');
const vm=require('node:vm');
const page=fs.readFileSync('src/http/HttpServerWebUi.inc','utf8');
const html=page.match(/<input type="search" id="tileSearchInput"[\s\S]*?>/);
assert.ok(html,'Missing tile search field');
const placeholders={};
for(const [,locale,example] of html[0].matchAll(/data-placeholder-(ru|en|hr|de)="([^"]*)"/g))
 placeholders[locale]=example;
assert.deepEqual(placeholders,{
 ru:'Например: Мир 24, 239.1.1.1',
 en:'e.g. Sport TV, 239.1.1.1',
 hr:'Npr. Sport TV, 239.1.1.1',
 de:'Z. B. Sport TV, 239.1.1.1'
});
assert.match(page,/\.language-picker select\{width:68px;min-width:68px;max-width:68px;/);
assert.match(page,/\.language-picker \.language-picker-label\{position:absolute;/);
for(const locale of ['ru','en','hr','de'])
 assert.match(page,new RegExp('<option value="'+locale+'">'+locale.toUpperCase()+'</option>'));
assert.match(page,/element\.id === 'tileSearchInput' \|\| element\.id === 'tileSearchClear'/);
const make=(id,attrs={})=>({id,attrs:{...attrs},textContent:'',title:'',value:'',placeholder:'',
 getAttribute(name){return this.attrs[name]??null;},
 setAttribute(name,value){this.attrs[name]=String(value);}});
const fields=Object.fromEntries(['languageSelect','languagePickerLabel','tileSearchInput','tileSearchClear','tileSearchEmpty'].map(id=>[id,make(id)]));
for(const [locale,example] of Object.entries(placeholders))
 fields.tileSearchInput.attrs['data-placeholder-'+locale]=example;
const box=make('headerSearchBox'),stored=[];
const ctx={document:{documentElement:{lang:''},body:{},querySelectorAll:()=>[],
 getElementById:id=>fields[id]||null,querySelector:q=>q==='.header-tile-search'?box:null},
 localStorage:{setItem:(k,v)=>stored.push([k,v])},applyExplicitBilingual:()=>{},
 applyRuntimeLocalization:()=>{},t:k=>k,updateDashboardThemeButton:()=>{},
 render:()=>{},saveLanguagePreference:()=>{}};
const from=page.indexOf('function applyLanguage() {');
const to=page.indexOf('function closeSystemMenu() {',from);
assert.ok(from>0&&to>from,'Missing language handlers');
vm.createContext(ctx);
vm.runInContext("let language='en';function normalizeLanguage(v){return ['ru','en','hr','de'].includes(v)?v:'en';}\n"+page.slice(from,to),ctx,{timeout:3000});
const labels={ru:'Поиск канала, устройства, IP, домена или подсети',
 en:'Search channel, device, IP, domain or subnet',
 hr:'Traži kanal, uređaj, IP, domenu ili podmrežu',
 de:'Kanal, Gerät, IP, Domain oder Subnetz suchen'};
for(const locale of ['ru','hr','de','en']){
 ctx.selectInterfaceLanguage(locale);
 assert.equal(ctx.document.documentElement.lang,locale);
 assert.equal(fields.languageSelect.value,locale);
 assert.equal(fields.tileSearchInput.placeholder,placeholders[locale]);
 assert.equal(fields.tileSearchInput.attrs['aria-label'],labels[locale]);
 assert.equal(box.attrs['aria-label'],labels[locale]);
}
assert.ok(stored.some(([k,v])=>k==='dvbstreamer5-language'&&v==='de'));
const version=fs.readFileSync('src/AppVersion.h','utf8').match(/kProgramVersion = "10\.8\.(\d+)"/);
assert.ok(version&&Number(version[1])>=581);
console.log('PASS V581: compact four-language selector, live localized search example and persistence');
