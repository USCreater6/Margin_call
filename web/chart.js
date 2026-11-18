/* Margin_call chart (owner-approved preview 18 Nov 2026). SIMULATION terminal.
   Library: TradingView Lightweight Charts(TM) v5.3.0 (web/vendor, Apache License 2.0; attribution on the chart and in
   the footer, as the licence asks). The library has no indicators or drawing tools: the ones here are computed by
   Margin_call.
   Data: intraday bars from the engine (/api/candles: replay path on real 1-minute candles, or the live prices recorded
   each second); daily bars from web/data/history5 (Upstox, 5 years) or web/data/history (NSE, 1 year).
   Uses the page's globals: I, S, RP, API, post, poll, inst, selSym, selectSym, H, inr, qN, openOF, toastMsg. */
(function(){
const LW=window.LightweightCharts;
const $=s=>document.querySelector(s),$$=s=>[...document.querySelectorAll(s)];
const TFS=[['1s',1],['5s',5],['10s',10],['30s',30],['1m',60],['5m',300],['15m',900],['1h',3600],['1D','D']];
const NB={1:1800,5:1500,10:1500,30:800,60:400,300:100,900:40,3600:10};
const RNG={'5D':5,'1M':21,'3M':63,'6M':126,'1Y':250,'5Y':1300};
let ch=null,main=null,vol=null,volMa=null,ext={},TF=60,CT='c',RANGE='1D',SYM='',DATA=[],DAY={},TOOL='x',PEND=null,DRAW={},timer=null,busy=false;
const IND=ld('mc_ind',{sma:false,ema:false,vwap:true,bb:false,vol:true,rsi:false,macd:false});
let INST=ld('mc_inst',true),SCAL=false;
function ld(k,d){try{const v=JSON.parse(localStorage.getItem(k));return v==null?d:v}catch(e){return d}}
function save(k,v){try{localStorage.setItem(k,JSON.stringify(v))}catch(e){}}
const css=n=>getComputedStyle(document.body).getPropertyValue(n).trim();
const f2=x=>x==null||!isFinite(x)?'-':x.toLocaleString('en-IN',{minimumFractionDigits:2,maximumFractionDigits:2});
/* market seconds of the data day -> chart time (IST wall clock written as UTC, so the axis shows IST) */
const tOf=(date,sec)=>{const d=String(date||'2000-01-01');return Date.UTC(+d.slice(0,4),+d.slice(5,7)-1,+d.slice(8,10),9,15)/1000+Math.floor(sec)};
function init(){
  if(!LW||!$('#lwc'))return;
  $('#ctf').innerHTML=TFS.map(([n,v])=>'<button class="btn" data-ctf="'+v+'">'+n+'</button>').join('');
  $('#rng').innerHTML=['1D','5D','1M','3M','6M','1Y','5Y'].map(r=>'<button class="btn" data-rg="'+r+'">'+r+'</button>').join('');
  ch=LW.createChart($('#lwc'),{autoSize:true,layout:{attributionLogo:true,fontFamily:css('--mono')||'monospace',fontSize:11},crosshair:{mode:0},
    timeScale:{timeVisible:true,secondsVisible:true,rightOffset:4},localization:{priceFormatter:p=>f2(p)}});
  theme();
  ch.subscribeCrosshairMove(p=>{const d=p&&p.seriesData&&main?p.seriesData.get(main):null;legend(d?{o:d.open??d.value,h:d.high??d.value,l:d.low??d.value,c:d.close??d.value,v:(DATA.find(x=>x.time===p.time)||{}).v}:null)});
  ch.subscribeClick(click);
  $('#pChart').addEventListener('click',onClick);
  $('#ctype').addEventListener('change',e=>{CT=e.target.value;build();setData()});
  $('#indM').addEventListener('change',e=>{const k=e.target.dataset.i;if(!k)return;IND[k]=e.target.checked;save('mc_ind',IND);build();setData()});
  $$('#indM [data-i]').forEach(x=>x.checked=!!IND[x.dataset.i]);
  tog('#instT',INST);
  document.addEventListener('keydown',e=>{if(e.key==='Escape'&&$('#secC').classList.contains('maxed'))maxi(false)});
  build();mark();
}
function theme(){if(!ch)return;const bg=css('--panel'),ln=css('--line'),mu=css('--mute');
  ch.applyOptions({layout:{background:{color:bg},textColor:mu,panes:{separatorColor:ln}},grid:{vertLines:{color:css('--row')},horzLines:{color:css('--row')}},rightPriceScale:{borderColor:ln},timeScale:{borderColor:ln}})}
function tog(sel,on){const t=$(sel);if(t)t.classList.toggle('on',!!on)}
function mark(){$$('#ctf button').forEach(b=>b.classList.toggle('on',String(TF)===b.dataset.ctf));$$('#rng button').forEach(b=>b.classList.toggle('on',b.dataset.rg===RANGE))}
/* series: rebuilt when the chart type or the indicators change; data is set on every refresh */
function build(){
  if(!ch)return;
  Object.values(ext).forEach(s=>{try{ch.removeSeries(s)}catch(e){}});ext={};
  [main,vol,volMa].forEach(s=>{if(s)try{ch.removeSeries(s)}catch(e){}});main=vol=volMa=null;
  const up=css('--good'),dn=css('--bad');
  main=CT==='l'?ch.addSeries(LW.LineSeries,{color:css('--accent'),lineWidth:2}):ch.addSeries(CT==='b'?LW.BarSeries:LW.CandlestickSeries,CT==='b'?{upColor:up,downColor:dn}:{upColor:up,downColor:dn,borderVisible:false,wickUpColor:up,wickDownColor:dn});
  const ln=(c,p=0)=>ch.addSeries(LW.LineSeries,{color:c,lineWidth:1,priceLineVisible:false,lastValueVisible:false,crosshairMarkerVisible:false},p);
  if(IND.sma)ext.sma=ln('#e8c14a');if(IND.ema)ext.ema=ln('#c77dff');if(IND.vwap)ext.vwap=ln('#ff9f43');
  if(IND.bb){ext.bbu=ln('#5fa8d3');ext.bbl=ln('#5fa8d3')}
  if(IND.vol){vol=ch.addSeries(LW.HistogramSeries,{priceFormat:{type:'volume'},priceScaleId:'v',lastValueVisible:false,priceLineVisible:false});vol.priceScale().applyOptions({scaleMargins:{top:.8,bottom:0}});
    volMa=ch.addSeries(LW.LineSeries,{color:css('--mute'),lineWidth:1,priceScaleId:'v',priceLineVisible:false,lastValueVisible:false,crosshairMarkerVisible:false})}
  let pane=1;if(IND.rsi)ext.rsi=ln('#c77dff',pane++);if(IND.macd){ext.macd=ln(css('--accent'),pane);ext.sig=ln('#ff9f43',pane);pane++}
  try{const ps=ch.panes();for(let k=ps.length-1;k>=pane;k--)if(k>0)ch.removePane(k);ch.panes().forEach((p,k)=>{if(p.setStretchFactor)p.setStretchFactor(k?1:4)})}catch(e){}
  redraw();
}
const sma=(a,n)=>a.map((_,i)=>i<n-1?null:a.slice(i-n+1,i+1).reduce((x,y)=>x+y,0)/n);
const ema=(a,n)=>{const k=2/(n+1);let e=null;return a.map(v=>e=e==null?v:v*k+e*(1-k))};
function setData(){
  if(!main)return;const B=DATA,tm=B.map(b=>b.time),cl=B.map(b=>b.close),put=(s,v)=>{if(s)s.setData(v.map((x,i)=>x==null||!isFinite(x)?{time:tm[i]}:{time:tm[i],value:x}))};
  main.setData(CT==='l'?B.map(b=>({time:b.time,value:b.close})):B.map(b=>({time:b.time,open:b.open,high:b.high,low:b.low,close:b.close})));
  put(ext.sma,sma(cl,20));put(ext.ema,ema(cl,20));
  if(ext.vwap){let pv=0,vv=0,day='';put(ext.vwap,TF==='D'?B.map(()=>null):B.map(b=>{pv+=(b.high+b.low+b.close)/3*(b.v||0);vv+=b.v||0;return vv?pv/vv:null}))}
  if(ext.bbu){const m=sma(cl,20),sd=cl.map((_,i)=>i<19?null:Math.sqrt(cl.slice(i-19,i+1).reduce((a,x)=>a+(x-m[i])**2,0)/20));put(ext.bbu,m.map((x,i)=>x==null?null:x+2*sd[i]));put(ext.bbl,m.map((x,i)=>x==null?null:x-2*sd[i]))}
  if(vol){vol.setData(B.map(b=>({time:b.time,value:b.v||0,color:b.close>=b.open?'rgba(63,191,138,.45)':'rgba(239,93,93,.45)'})));volMa.setData(sma(B.map(b=>b.v||0),20).map((x,i)=>x==null?{time:tm[i]}:{time:tm[i],value:x}))}
  if(ext.rsi){let g=0,l=0;put(ext.rsi,cl.map((c,i)=>{if(!i)return null;const d=c-cl[i-1];g=(g*13+Math.max(d,0))/14;l=(l*13+Math.max(-d,0))/14;return i<14?null:100-100/(1+(l?g/l:100))}))}
  if(ext.macd){const a=ema(cl,12),b=ema(cl,26),m=a.map((x,i)=>x-b[i]);put(ext.macd,m);put(ext.sig,ema(m,9))}
  legend(null);
}
function redraw(){if(!main)return;(DRAW[SYM]||[]).forEach(d=>{if(d.k==='hl')d.pl=main.createPriceLine({price:d.p,color:'#e8c14a',lineWidth:1,lineStyle:2,axisLabelVisible:true,title:'line'});
  else if(d.k==='tl'&&d.tf===String(TF)){const s=ch.addSeries(LW.LineSeries,{color:'#e8c14a',lineWidth:2,priceLineVisible:false,lastValueVisible:false,crosshairMarkerVisible:false});s.setData(d.pts);ext['tl'+Math.random()]=s}})}
/* data */
async function fetchDaily(sym){
  if(DAY[sym])return DAY[sym];
  const rd=async u=>{try{const r=await fetch(u,{cache:'no-store'});if(!r.ok)return[];return(await r.text()).split('\n').filter(l=>/^\d{8},/.test(l)).map(l=>{const c=l.split(',');return{time:c[0].slice(0,4)+'-'+c[0].slice(4,6)+'-'+c[0].slice(6,8),open:+c[1],high:+c[2],low:+c[3],close:+c[4],v:+c[5]}})}catch(e){return[]}};
  let d=await rd('data/history5/'+encodeURIComponent(sym)+'.csv'),src='Upstox daily candles, 5 years';
  if(!d.length){d=await rd('data/history/'+encodeURIComponent(sym)+'.csv');src='NSE daily files, 1 year'}
  return DAY[sym]={rows:d,src};
}
async function load(){
  const sym=typeof selSym==='function'?selSym():'';if(!ch||!sym||busy||!API||(typeof CX!=='undefined'&&CX!=='chart'))return;
  busy=true;
  try{
    if(sym!==SYM){SYM=sym;build()}
    const tf=TF==='D'?60:TF;
    const r=await fetch('api/candles?sym='+encodeURIComponent(sym)+'&tf='+tf+'&n='+(TF==='D'?400:NB[TF]||400),{cache:'no-store'});
    const j=r.ok?await r.json():{bars:[]};
    let rows=(j.bars||[]).map(b=>({time:tOf(j.date,b[0]),open:b[1],high:b[2],low:b[3],close:b[4],v:b[5]||0}));
    if(TF==='D'){const D=await fetchDaily(sym),today=String(j.date||'');const d=D.rows.filter(x=>x.time!==today);
      if(rows.length)d.push({time:today,open:rows[0].open,high:Math.max(...rows.map(x=>x.high)),low:Math.min(...rows.map(x=>x.low)),close:rows[rows.length-1].close,v:rows.reduce((a,x)=>a+x.v,0)});
      rows=d;NOTE=(D.rows.length?D.src:'no daily history for '+sym)+'; last bar: '+today+' so far';}
    else NOTE=j.mode==='live'?'Live: prices recorded each second since the market opened'+(TF<60?' (one price a second)':''):j.real?'Replay of '+j.date+' on real 1-minute candles (Upstox): every minute\'s open, high, low and close are real'+(TF<60?'; the moves inside a minute are simulated':''):'Replay of '+j.date+': official open, high, low, close; the moves between them are simulated';
    const first=!DATA.length||DATA.sym!==sym+'|'+TF;
    DATA=rows;DATA.sym=sym+'|'+TF;setData();
    if(first){if(TF==='D'&&RNG[RANGE]){ch.timeScale().setVisibleLogicalRange({from:Math.max(0,rows.length-RNG[RANGE]),to:rows.length+2})}else ch.timeScale().fitContent()}
    $('#cNote').textContent=NOTE;
  }catch(e){}finally{busy=false}
  sched();
}
let NOTE='';
function sched(){clearTimeout(timer);timer=setTimeout(load,TF!=='D'&&TF<60?1000:TF==='D'?10000:2000)}
function legend(b){const i=(typeof I!=='undefined'&&I[S.sel])||{};const last=DATA[DATA.length-1];b=b||(last?{o:last.open,h:last.high,l:last.low,c:last.close,v:last.v}:null);
  const c=i.pc&&b?b.c-i.pc:0,bb=i.bb&&i.bb[0]?i.bb[0]:i.p,ba=i.ba&&i.ba[0]?i.ba[0]:i.p,L=i.lot||1;
  $('#cDay').innerHTML=b?'O <b>'+f2(b.o)+'</b> H <b>'+f2(b.h)+'</b> L <b>'+f2(b.l)+'</b> C <b>'+f2(b.c)+'</b> <span class="'+(c>=0?'up':'dn')+'">'+(c>=0?'+':'')+f2(c)+(i.pc?' ('+(c/i.pc*100).toFixed(2)+'%)':'')+'</span>'+(b.v?' Vol <b>'+Math.round(b.v).toLocaleString('en-IN')+'</b>':''):'';
  $('#oSell').textContent='SELL '+f2(bb);$('#oBuy').textContent='BUY '+f2(ba);$('#oQ').title=L>1?'Lots (1 lot = '+L+')':'Quantity';}
/* drawing tools */
function click(p){if(!p||!p.point||!main||TOOL==='x')return;const price=main.coordinateToPrice(p.point.y),t=p.time;if(price==null)return;
  const L=DRAW[SYM]=DRAW[SYM]||[];
  if(TOOL==='hl'){L.push({k:'hl',p:price});build();setData();return}
  if(t==null)return;if(!PEND){PEND={t,price};msg((TOOL==='tl'?'Trend line':'Measure')+': click the second point');return}
  const a=PEND,b={t,price};PEND=null;if(a.t===b.t)return;
  if(TOOL==='tl'){L.push({k:'tl',tf:String(TF),pts:[a,b].sort((x,y)=>x.t>y.t?1:-1).map(x=>({time:x.t,value:x.price}))});build();setData()}
  else{const d=b.price-a.price;msg('Measure: '+(d>=0?'+':'')+f2(d)+' ('+(d/a.price*100).toFixed(2)+'%)')}}
function msg(t){const m=$('#cMsg');if(!m)return;m.textContent=t;clearTimeout(m._t);m._t=setTimeout(()=>m.textContent='',4000)}
/* orders from the chart */
function chartOrder(side){
  const s=selSym(),i=inst(s),L=i.lot||1,n=Math.floor(+$('#oQ').value);
  if(!INST){openOF(side,s);return}
  if(!(n>=1)){msg('Enter a quantity of 1 or more');return}
  const prod=window.ORD_PROD||'Intraday';
  post('order',{sym:s,side,type:'MARKET',qty:n*L,px:0,sl:0,tp:0,prod:prod==='MTF'?'Intraday':prod}).then(r=>{msg(r.ok?(r.filled>=r.qty?'Filled '+qN(r.filled,s)+' '+s+' at avg '+r.avg.toFixed(2)+' (instant order, '+(prod==='MTF'?'Intraday':prod)+')':'Order #'+r.id+': '+r.st):'Rejected: '+r.why);poll()})}
/* scalper: options of the underlying on the NIFTY-FUT chart; MARKET orders only, buy only */
function scalper(){
  const box=$('#scal');if(!SCAL){box.classList.remove('on');return}
  box.classList.add('on');
  const opts=I.filter(i=>i.opt&&!i.opt.x),ix=S.index||0;
  if(!opts.length){box.innerHTML='<b>Scalper</b><p class="note">No NIFTY options loaded (the options file is fetched when the Codespace starts).</p>';return}
  const ks=[...new Set(opts.map(i=>i.opt.k))].sort((a,b)=>a-b),atm=ks.reduce((a,k)=>Math.abs(k-ix)<Math.abs(a-ix)?k:a,ks[0]),z=ks.indexOf(atm),show=ks.slice(Math.max(0,z-3),z+4);
  const exp=opts[0].opt.exp,lot=opts[0].lot||1,f=x=>x==null?'-':f2(x);
  const get=(k,cp)=>opts.find(i=>i.opt.k===k&&i.opt.cp===cp);
  const keep=id=>($('#'+id)||{}).value;
  const v={lots:keep('scL')||'1',sl:keep('scSl')||'',tp:keep('scTp')||'',tsl:keep('scTs')||'',u:keep('scU')||'%'};
  box.innerHTML='<div class="row" style="gap:6px;align-items:center"><b>Scalper · NIFTY</b><span class="note">expiry '+H(exp)+' · lot '+lot+'</span><span style="flex:1"></span><span class="pill sim">SIM</span></div>'
   +'<div class="sg"><label>Lots<input id="scL" value="'+H(v.lots)+'"></label><label>SL<input id="scSl" value="'+H(v.sl)+'" placeholder="-" title="Your rule: at most 2%"></label><label>TP<input id="scTp" value="'+H(v.tp)+'" placeholder="-"></label><label>TSL<input id="scTs" value="'+H(v.tsl)+'" placeholder="-"></label>'
   +'<label>In<select id="scU"><option'+(v.u==='%'?' selected':'')+'>%</option><option'+(v.u==='points'?' selected':'')+'>points</option></select></label><label style="grid-column:span 3">&nbsp;<button class="btn sell" data-scx="1" style="height:21px">Exit all scalper positions</button></label></div>'
   +'<table><tr><th>CE</th><th></th><th style="text-align:center">Strike</th><th></th><th>PE</th></tr>'+show.map(k=>{const c=get(k,'CE'),p=get(k,'PE');return'<tr'+(k===atm?' class="atm"':'')+'><td>'+f(c&&c.p)+'</td><td>'+(c?'<button class="btn sm buy" data-scb="'+H(c.s)+'">BUY</button>':'')+'</td><td class="mono" style="text-align:center"><b>'+k.toLocaleString('en-IN')+'</b>'+(k===atm?'<span class="note"> ATM</span>':'')+'</td><td>'+(p?'<button class="btn sm buy" data-scb="'+H(p.s)+'">BUY</button>':'')+'</td><td>'+f(p&&p.p)+'</td></tr>'}).join('')+'</table>'
   +'<p class="note" id="scMsg">Market orders only · product Scalper · buy only (no option writing) · every order through the risk engine. Prices: the engine\'s option prices.</p>';
}
function scBuy(sym){const i=inst(sym),n=Math.floor(+$('#scL').value),u=$('#scU').value,toPct=x=>{x=+x||0;return u==='points'?(i.p>0?x/i.p*100:0):x};
  const sl=toPct($('#scSl').value),tp=toPct($('#scTp').value),tsl=toPct($('#scTs').value);
  if(!(n>=1)){$('#scMsg').textContent='Enter 1 lot or more';return}
  post('order',{sym,side:'BUY',type:'MARKET',qty:n*(i.lot||1),px:0,sl:+sl.toFixed(2),tp:+tp.toFixed(2),tsl:+tsl.toFixed(2),prod:'Scalper'}).then(r=>{$('#scMsg').textContent=r.ok?'Bought '+qN(r.filled,sym)+' '+sym+' at avg '+r.avg.toFixed(2)+' (Scalper)':'Rejected: '+r.why;poll()})}
function scExit(){const ids=(S.lots||[]).filter(l=>l.prod==='Scalper'&&!l.c).map(l=>l.id);if(!ids.length){$('#scMsg').textContent='No open scalper positions';return}
  post('close',{ids:ids.join(',')}).then(r=>{$('#scMsg').textContent=r.ok?ids.length+' scalper position(s) closing at market':'Rejected: '+r.why;poll()})}
function maxi(on){const c=$('#secC');c.classList.toggle('maxed',on);$('#fullB').textContent=on?'Exit full screen':'⛶ Full screen'}
function onClick(e){const t=e.target;
  const tf=t.closest('[data-ctf]');if(tf){TF=tf.dataset.ctf==='D'?'D':+tf.dataset.ctf;if(TF!=='D')RANGE='1D';else if(RANGE==='1D')RANGE='6M';DATA=[];mark();build();load();return}
  const rg=t.closest('[data-rg]');if(rg){RANGE=rg.dataset.rg;if(RANGE==='1D'){if(TF==='D')TF=60}else TF='D';DATA=[];mark();build();load();return}
  const d=t.closest('[data-tool]');if(d){if(d.dataset.tool==='clr'){DRAW[SYM]=[];PEND=null;build();setData();return}TOOL=d.dataset.tool;PEND=null;$$('#draw [data-tool]').forEach(b=>b.classList.toggle('on',b===d));if(TOOL!=='x')msg({tl:'Trend line: click two points',hl:'Horizontal line: click a price',ms:'Measure: click two points'}[TOOL]);return}
  if(t.closest('#indB')){$('#indM').classList.toggle('on');return}
  if(!t.closest('#indM'))$('#indM').classList.remove('on');
  if(t.closest('#instT')){INST=!INST;save('mc_inst',INST);tog('#instT',INST);msg(INST?'Instant order on: chart BUY / SELL send a MARKET order at once':'Instant order off: chart BUY / SELL open the order form');return}
  if(t.closest('#scalT')){SCAL=!SCAL;tog('#scalT',SCAL);if(SCAL&&inst('NIFTY-FUT').s)selectSym('NIFTY-FUT');scalper();return}
  if(t.closest('#fullB')){maxi(!$('#secC').classList.contains('maxed'));return}
  const o=t.closest('[data-co]');if(o){chartOrder(o.dataset.co);return}
  const sb=t.closest('[data-scb]');if(sb){scBuy(sb.dataset.scb);return}
  if(t.closest('[data-scx]')){scExit();return}
  const sc=t.closest('#pctB,#logB,#autoB');if(sc){sc.classList.toggle('on');ch.priceScale('right').applyOptions({mode:$('#logB').classList.contains('on')?1:$('#pctB').classList.contains('on')?2:0,autoScale:$('#autoB').classList.contains('on')});return}
}
window.MCC={init,load:()=>{clearTimeout(timer);load()},render:()=>{if(ch&&selSym()!==SYM&&(typeof CX==='undefined'||CX==='chart')){DATA=[];clearTimeout(timer);load()}legend(null);const fb=$('#fullB');if(fb)fb.textContent=$('#secC').classList.contains('maxed')?'Exit full screen':'⛶ Full screen';if(SCAL&&!document.activeElement.closest('#scal'))scalper()},theme,scalperOn:()=>SCAL};
})();
