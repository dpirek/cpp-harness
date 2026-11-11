// Local stdio fixture. Stdout contains only newline-delimited MCP messages.
const readline = require('node:readline');
const send = message => process.stdout.write(JSON.stringify({jsonrpc:'2.0', ...message})+'\n');
let initialized = false;
readline.createInterface({input:process.stdin}).on('line', line => {
  const request=JSON.parse(line);
  if(!request.method)return;
  if(request.method==='initialize') {
    send({id:request.id,result:{protocolVersion:'2025-11-25',capabilities:{tools:{}},serverInfo:{name:'fixture',version:'1'}}});
    return;
  }
  if(request.method==='notifications/initialized'){initialized=true;return;}
  if(!initialized){send({id:request.id,error:{code:-32000,message:'Not initialized'}});return;}
  if(request.method==='tools/list') {
    process.stderr.write('MCP fixture diagnostic must not pollute stdout.\n');
    send({method:'notifications/message',params:{level:'info',data:'fixture'}});
    if(!request.params.cursor)send({id:request.id,result:{tools:[{name:'echo',description:'Echo text',inputSchema:{type:'object',properties:{text:{type:'string'}},required:['text']}}],nextCursor:'page-two'}});
    else send({id:request.id,result:{tools:[{name:'slow',description:'Wait until cancelled',inputSchema:{type:'object'}}]}});
    return;
  }
  if(request.method==='tools/call') {
    if(request.params.name==='slow'){setTimeout(()=>send({id:request.id,result:{content:[{type:'text',text:'late'}]}}),60000);return;}
    send({method:'ping',id:'server-ping'});
    send({id:request.id,result:{content:[{type:'text',text:request.params.arguments.text+' '+(process.env.MCP_FIXTURE_VALUE||'')}],isError:false}});
    return;
  }
  if(request.id)send({id:request.id,error:{code:-32601,message:'Unknown method'}});
}).on('close',()=>process.exit(0));
