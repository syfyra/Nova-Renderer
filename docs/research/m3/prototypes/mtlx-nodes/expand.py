import xml.etree.ElementTree as ET, sys, collections, json
L=r'C:/Program Files/Side Effects Software/Houdini 22.0.466/houdini/materialx/libraries/'
def load(p): return ET.parse(L+p).getroot()
docs={'std':('stdlib/stdlib_defs.mtlx','stdlib/stdlib_ng.mtlx'),'pbr':('pbrlib/pbrlib_defs.mtlx','pbrlib/pbrlib_ng.mtlx'),'cm':('cmlib/cmlib_defs.mtlx','cmlib/cmlib_ng.mtlx')}
nodedefs={}; ngs={}
for k,(d,n) in docs.items():
    for nd in load(d).iter('nodedef'): nodedefs[nd.get('name')]=(k,nd)
    for g in load(n).iter('nodegraph'): ngs[g.get('nodedef')]=g
for f in ['bxdf/standard_surface.mtlx','bxdf/open_pbr_surface.mtlx']:
    r=load(f)
    for nd in r.iter('nodedef'): nodedefs[nd.get('name')]=('bxdf',nd)
    for g in r.iter('nodegraph'):
        if g.get('nodedef'): ngs[g.get('nodedef')]=g
# map (node, output type, input types) -> nodedef: resolve node instances inside graphs
by_node=collections.defaultdict(list)
for name,(k,nd) in nodedefs.items(): by_node[nd.get('node')].append(name)
def outtype(nd):
    outs=nd.findall('output'); return outs[0].get('type') if outs else None
def resolve(node_el):
    cat=node_el.tag; t=node_el.get('type')
    nd_name=node_el.get('nodedef')
    if nd_name: return nd_name
    cands=[n for n in by_node.get(cat,[]) if outtype(nodedefs[n][1])==t or (t=='multioutput')]
    if len(cands)==1: return cands[0]
    # match input types
    ins={i.get('name'):i.get('type') for i in node_el.findall('input')}
    best=[]
    for c in cands:
        ndins={i.get('name'):i.get('type') for i in nodedefs[c][1].findall('input')}
        if all(n in ndins and ndins[n]==tt for n,tt in ins.items()): best.append(c)
    if len(best)>=1: return best[0]
    return cands[0] if cands else 'UNRESOLVED:'+cat+':'+str(t)
memo={}
def prims(ndname, stack=()):
    if ndname in memo: return memo[ndname]
    if ndname not in ngs: memo[ndname]={ndname}; return memo[ndname]
    s=set()
    for el in ngs[ndname]:
        if el.tag in ('input','output','nodedef'): continue
        if el.get('type') is None and el.tag not in by_node: continue
        r=resolve(el)
        s|=prims(r)
    memo[ndname]=s; return s
target=sys.argv[1] if len(sys.argv)>1 else 'std'
allp=set(); 
std_names=[n for n,(k,nd) in nodedefs.items() if k=='std']
for n in std_names: allp|=prims(n)
print('stdlib nodedefs',len(std_names),'of which NG',sum(1 for n in std_names if n in ngs))
print('primitive nodedefs after expanding all stdlib:',len(allp))
nodes=collections.Counter(nodedefs[p][1].get('node') if p in nodedefs else p for p in allp)
print('primitive node categories:',len(nodes))
# primitives reachable only from NGs
ngp=set()
for n in std_names:
    if n in ngs: ngp|=prims(n)
print('primitive nodedefs used by stdlib NGs:',len(ngp))
print(sorted(ngp))
for top in ['ND_standard_surface_surfaceshader','ND_open_pbr_surface_surfaceshader']:
    p=prims(top); print(top, len(p)); print(sorted(p))
json.dump({k:sorted(v) for k,v in memo.items()}, open(sys.argv[0]+'.json','w'), indent=0)
