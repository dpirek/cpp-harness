// Loopback-only fixtures. No credentials or public network access required.
const http = require('node:http');
const { spawn } = require('node:child_process');
const path = require('node:path');
let attachmentRequests = 0;
const server = http.createServer((req, res) => {
  let data = '';
  req.setEncoding('utf8');
  req.on('data', chunk => data += chunk);
  req.on('end', () => {
    if(req.method==='GET') {
      if(req.url==='/keyless/models') {res.writeHead(req.headers.authorization===undefined?200:400);res.end('{"data":[{"id":"local-model"}]}');return;}
      if(req.headers.authorization!=='Bearer test-key'){res.writeHead(401).end();return;}
      if(req.url==='/v1/models'){res.setHeader('Content-Type','application/json');res.end(JSON.stringify({data:[{id:'z/model'},{id:'a/model'},{id:'z/model'},{id:''},{name:'invalid'}]}));return;}
      if(req.url==='/invalid/models'){res.end('{broken');return;}
      if(req.url==='/empty/models'){res.end('{"data":[]}');return;}
      if(req.url==='/slow/models'){setTimeout(()=>res.end('{"data":[{"id":"slow"}]}'),1500);return;}
      res.writeHead(404).end();return;
    }
    let payload;
    try { payload = JSON.parse(data); } catch { res.writeHead(400).end(); return; }
    if(req.url==='/mcp') {
      if(req.headers.authorization!=='Bearer mcp-token'){res.writeHead(401).end();return;}
      if(payload.method==='initialize') {
        res.setHeader('Mcp-Session-Id','fixture-session');res.setHeader('Content-Type','application/json');
        res.end(JSON.stringify({jsonrpc:'2.0',id:payload.id,result:{protocolVersion:'2025-11-25',capabilities:{tools:{}},serverInfo:{name:'remote-fixture',version:'1'}}}));return;
      }
      if(req.headers['mcp-session-id']!=='fixture-session'||req.headers['mcp-protocol-version']!=='2025-11-25'){res.writeHead(400).end();return;}
      if(!payload.id){res.writeHead(202).end();return;}
      const result=payload.method==='tools/list'?{tools:[{name:'echo',description:'Remote echo',inputSchema:{type:'object',properties:{text:{type:'string'}}}}]}:payload.params.arguments.text==='structured'?{content:[{type:'text',text:'Structured output'}],structuredContent:{kind:'mcp',enabled:true},isError:false}:{content:[{type:'text',text:payload.params.arguments.text}],isError:false};
      res.setHeader('Content-Type','text/event-stream');
      res.write(': keepalive\r\n\r\ndata: '+JSON.stringify({jsonrpc:'2.0',method:'notifications/message',params:{level:'info',data:'hello'}})+'\r\n\r\n');
      const response='data: '+JSON.stringify({jsonrpc:'2.0',id:payload.id,result})+'\r\n\r\n';
      res.write(response.slice(0,15));setTimeout(()=>res.end(response.slice(15)),10);return;
    }
    if(req.url==='/keyless/chat/completions') {
      res.writeHead(req.headers.authorization===undefined&&payload.model==='test/model'?200:400,{'Content-Type':'text/event-stream'});
      res.end('data: {"choices":[{"delta":{"content":"Local reply"}}]}\n\ndata: [DONE]\n\n');return;
    }
    if (payload.stream !== true || payload.model !== 'test/model' || req.headers.authorization !== 'Bearer test-key' || payload.messages.some(m => m.role === 'error')) {
      res.writeHead(400).end(); return;
    }
    const delta = content => 'data: ' + JSON.stringify({choices:[{index:0,delta:{content}}]}) + '\n\n';
    const done = 'data: '+JSON.stringify({choices:[],usage:{prompt_tokens:120,completion_tokens:30}})+'\n\ndata: [DONE]\n\n';
    const body = delta('Native reply 🚀') + done;
    if(req.url==='/attachments/chat/completions') {
      const content=payload.messages.at(-1).content;
      if(!Array.isArray(content)||content.length!==2||!content[0].text.includes('Dropped document fixture')||!content[1].image_url.url.startsWith('data:image/png;base64,')){res.writeHead(400).end();return;}
      attachmentRequests++;res.end(delta('Attachments received')+done);return;
    }
    const toolStream = calls => {
      // Deliberately split each arguments string across separate SSE frames.
      const first = calls.map((call,index) => ({index,id:call.id,type:'function',function:{name:call.name,arguments:call.arguments.slice(0,8)}}));
      const rest = calls.map((call,index) => ({index,function:{arguments:call.arguments.slice(8)}}));
      return 'data: '+JSON.stringify({choices:[{index:0,delta:{tool_calls:first}}]})+'\n\n'+
        'data: '+JSON.stringify({choices:[{index:0,delta:{tool_calls:rest},finish_reason:'tool_calls'}]})+'\n\n'+done;
    };
    res.setHeader('Content-Type', 'text/event-stream');
    switch (req.url) {
      case '/streaming/chat/completions':
        res.end(body);break;
      case '/markdown/chat/completions':
        res.end(delta('# Formatted response\n\n**Bold** and *italic*, `inline code`, [link](https://example.com).\n\n- Bullet item\n- [x] Task done\n\n> Quoted text.\n\n| Name | Value |\n| --- | --- |\n| XML | Yes |\n\n```xml\n<root>value</root>\n```\n')+done);
        break;
      case '/agent-computer-input-json':
      case '/agent-computer-input-stream': {
        const fixture=JSON.parse(payload.messages.find(m=>m.role==='user').content);
        const outputs=payload.messages.filter(m=>m.role==='tool');
        const calls=[
          {id:'move',name:'computer',arguments:JSON.stringify({action:'move',x:fixture.x,y:fixture.y})},
          {id:'click',name:'computer',arguments:JSON.stringify({action:'click',x:fixture.x,y:fixture.y})},
          {id:'type',name:'computer',arguments:JSON.stringify({action:'type',text:'replace me'})},
          {id:'key',name:'computer',arguments:JSON.stringify({action:'key',keys:['SHIFT','HOME']})},
          {id:'replace',name:'computer',arguments:JSON.stringify({action:'type',text:'Model input delivered'})}
        ];
        if(outputs.length) {
          if(outputs.length!==calls.length||outputs.some((o,i)=>o.tool_call_id!==calls[i].id||JSON.parse(o.content).ok!==true)){res.writeHead(400).end();break;}
          res.end(delta('Input complete')+done);
        } else if(req.url.endsWith('-json')) {
          res.setHeader('Content-Type','application/json');
          res.end(JSON.stringify({choices:[{message:{role:'assistant',content:null,tool_calls:calls.map(c=>({id:c.id,type:'function',function:{name:c.name,arguments:c.arguments}}))}}]}));
        } else res.end(toolStream(calls));
        break;
      }
      case '/agent-computer': {
        const outputs=payload.messages.filter(m=>m.role==='tool');
        if(!payload.tools.some(t=>t.function.name==='computer')){res.writeHead(400).end();break;}
        if(!outputs.length)res.end(toolStream([{id:'screen',name:'computer',arguments:'{"action":"screenshot"}'}]));
        else {
          const image=payload.messages.at(-1);
          const bounds=JSON.parse(outputs.at(-1).content);
          if(outputs.at(-1).tool_call_id!=='screen'||!bounds.width||image.role!=='user'||!image.content?.[1]?.image_url?.url.startsWith('data:image/png;base64,iVBOR'))res.writeHead(400).end();
          else res.end(delta('Desktop received')+done);
        }
        break;
      }
      case '/agent-mcp': {
        const tool=payload.tools.find(t=>t.function.name.startsWith('mcp_'));
        const output=payload.messages.find(m=>m.role==='tool');
        if(!tool){res.writeHead(400).end();break;}
        if(!output)res.end(toolStream([{id:'mcp-call',name:tool.function.name,arguments:JSON.stringify({text:'MCP loop'})}]));
        else if(output.tool_call_id==='mcp-call'&&JSON.parse(output.content).content[0].text==='MCP loop')res.end(delta('MCP completed')+done);
        else res.writeHead(400).end();
        break;
      }
      case '/agent-formats': {
        const tool=payload.tools.find(t=>t.function.name.startsWith('mcp_'));
        const outputs=payload.messages.filter(m=>m.role==='tool');
        if(!tool){res.writeHead(400).end();break;}
        if(!outputs.length)res.end(toolStream([
          {id:'ps-json',name:'powershell',arguments:JSON.stringify({command:'Write-Output \'{"kind":"powershell","enabled":true}\''})},
          {id:'mcp-xml',name:tool.function.name,arguments:JSON.stringify({text:'<root>mcp</root>'})},
          {id:'mcp-json',name:tool.function.name,arguments:JSON.stringify({text:'structured'})}
        ]));
        else {
          try {
            if(outputs.length===3&&JSON.parse(JSON.parse(outputs[0].content).output).kind==='powershell'&&JSON.parse(outputs[1].content).content[0].text==='<root>mcp</root>'&&JSON.parse(outputs[2].content).structuredContent.kind==='mcp')res.end(delta('Formats complete')+done);
            else res.writeHead(400).end();
          }catch(error){console.error('Structured-output fixture failed:',outputs[0].content);res.writeHead(400).end();}
        }
        break;
      }
      case '/agent': {
        const tools = payload.messages.filter(m => m.role === 'tool');
        if (!tools.length) {
          res.end(toolStream([
            {id:'call_one',name:'powershell',arguments:JSON.stringify({command:"Set-Content -LiteralPath 'agent-output.txt' -Value 'agent-tool-ok'; Get-Content -LiteralPath 'agent-output.txt'",timeout_seconds:10})},
            {id:'call_two',name:'powershell',arguments:JSON.stringify({command:"Write-Output 'second-tool'"})}
          ]));
        } else {
          const assistant = payload.messages.find(m => m.tool_calls?.length);
          const outputs = tools.map(m => JSON.parse(m.content));
          if (tools.length !== 2 || assistant?.tool_calls.length !== 2 || tools[0].tool_call_id !== 'call_one' || tools[1].tool_call_id !== 'call_two' || outputs.some(o => o.exit_code !== 0) || !outputs[0].output.includes('agent-tool-ok') || !outputs[1].output.includes('second-tool')) {
            res.writeHead(400).end();break;
          }
          res.end(delta('Agent ') + delta('finished') + done);
        }
        break;
      }
      case '/badtool':
      case '/badargs': {
        const tools = payload.messages.filter(m => m.role === 'tool');
        if (!tools.length) res.end(toolStream([{id:'bad_call',name:req.url === '/badtool'?'unknown':'powershell',arguments:'not-json'}]));
        else if (JSON.parse(tools.at(-1).content).error) res.end(delta('Recovered')+done);
        else res.writeHead(400).end();
        break;
      }
      case '/loop': res.end(toolStream([{id:'loop_'+payload.messages.length,name:'unknown',arguments:'{}'}])); break;
      case '/unauthorized': res.writeHead(401).end('private error details'); break;
      case '/attachment-rejected': res.writeHead(400).end('private details'); break;
      case '/attachment-too-large': res.writeHead(413).end('private details'); break;
      case '/malformed': res.end('{invalid'); break;
      case '/empty': res.end(done); break;
      case '/apierror': res.end('data: {"error":{"message":"private details"}}\n\n'); break;
      case '/oversized': res.end(':' + 'x'.repeat(4 * 1024 * 1024 + 1)); break;
      case '/redirect': res.writeHead(302, {Location:'/success'}).end(); break;
      case '/slow/chat/completions':
      case '/slow': {
        const timer = setTimeout(() => res.end(body), 10000);
        res.on('close', () => clearTimeout(timer));
        break;
      }
      case '/worker': {
        if (payload.messages.at(-1).content === 'slow') {
          const timer = setTimeout(() => res.end(body), 10000);
          res.on('close', () => clearTimeout(timer));
        } else res.end(body);
        break;
      }
      case '/streaming':
        res.write(delta('Native '));
        setTimeout(() => res.end(delta('reply 🚀') + done), 200);
        break;
      case '/interrupted': res.end(delta('Partial text')); break;
      case '/streamerror': res.end(delta('Partial text') + 'data: {"error":{"message":"private details"}}\n\n'); break;
      case '/wait-after-first': {
        res.write(delta('First token'));
        const timer = setTimeout(() => res.end(done), 10000);
        res.on('close', () => clearTimeout(timer));
        break;
      }
      case '/chunked':
        res.writeHead(200, {'Content-Type':'application/json'});
        res.write(body.slice(0, 20));
        setTimeout(() => res.end(body.slice(20)), 10);
        break;
      default: res.end(body);
    }
  });
});
server.listen(0, '127.0.0.1', () => {
  const ui=process.argv.includes('--ui');
  const executableFlag=process.argv.indexOf('--test-executable');
  const testExecutable=executableFlag<0?'build/chat-tests.exe':process.argv[executableFlag+1];
  if(!testExecutable){console.error('--test-executable requires a path');server.close();process.exitCode=1;return;}
  const child = ui ? spawn('powershell',['-NoProfile','-ExecutionPolicy','Bypass','-File','tests/ui_smoke.ps1','-Executable','build/chat-preview.exe','-ModelServerPort',String(server.address().port)],{stdio:'inherit',windowsHide:true}) : spawn(path.resolve(testExecutable), [String(server.address().port)], {stdio:'inherit', windowsHide:true});
  const timer = setTimeout(() => { child.kill(); console.error('Test deadline exceeded'); }, 30000);
  child.on('error', error => { console.error(error.message); clearTimeout(timer);server.close();process.exitCode=1; });
  child.on('exit', code => { clearTimeout(timer);server.closeAllConnections();server.close();if(ui&&attachmentRequests!==1){console.error('Expected one request with dropped attachment contents');process.exitCode=1;}else process.exitCode=code ?? 1; });
});
