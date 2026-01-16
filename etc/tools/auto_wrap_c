#!/usr/bin/env python3
import os,sys,re,json,platform,subprocess,ctypes.util

def sh(cmd,inp=None):
	return subprocess.check_output(cmd,input=inp,text=True,stderr=subprocess.STDOUT)

def envp(v):
	s=os.environ.get(v,"")
	if not s: return []
	sep=";" if os.name=="nt" else ":"
	return [p for p in s.split(sep) if p]

def cc_includes():
	cc=os.environ.get("CC","cc")
	try: out=sh([cc,"-E","-x","c","-","-v"],"")
	except Exception: return []
	inc=[]; g=False
	for l in out.splitlines():
		if "#include <...>" in l: g=True; continue
		if g:
			if not l.strip(): break
			p=l.strip()
			if os.path.isdir(p): inc.append(p)
	return inc

def pkg_list():
	try: return [l.split()[0] for l in sh(["pkg-config","--list-all"]).splitlines() if l.strip()]
	except Exception: return []

def pkg_cflags(p):
	try: return sh(["pkg-config","--cflags",p]).split()
	except Exception: return []

def pkg_libs(p):
	try: return sh(["pkg-config","--libs",p]).split()
	except Exception: return []

def split_cflags(ts):
	inc=[]; defs=[]
	for t in ts:
		if t.startswith("-I"): inc.append(t[2:])
		elif t.startswith("-D"): defs.append(t[2:])
	return inc,defs

def find_header(h):
	if os.path.isfile(h): return os.path.abspath(h),None,[]
	b=os.path.basename(h)
	paths=set()
	paths.update(envp("CPATH")); paths.update(envp("C_INCLUDE_PATH"))
	if os.name=="nt": paths.update(envp("INCLUDE"))
	paths.update(cc_includes())
	for p in ("/usr/include","/usr/local/include"):
		if os.path.isdir(p): paths.add(p)
	for p in list(paths):
		fp=os.path.join(p,b)
		if os.path.isfile(fp): return os.path.abspath(fp),None,[]
	guess=os.path.splitext(b)[0]
	for cand in [guess,guess.replace("_","-"),guess.replace("-","_")]:
		ts=pkg_cflags(cand)
		if not ts: continue
		inc,_=split_cflags(ts)
		for ip in inc:
			fp=os.path.join(ip,b)
			if os.path.isfile(fp): return os.path.abspath(fp),cand,ts
	for p in pkg_list():
		ts=pkg_cflags(p)
		if not ts: continue
		inc,_=split_cflags(ts)
		for ip in inc:
			fp=os.path.join(ip,b)
			if os.path.isfile(fp): return os.path.abspath(fp),p,ts
	return None,None,[]

def try_clang():
	try:
		from clang import cindex
		return cindex
	except Exception:
		return None

def clang_parse(cindex,hdr,inc,defs):
	idx=cindex.Index.create()
	args=["-x","c","-std=c11"]+[f"-I{p}" for p in inc]+[f"-D{d}" for d in defs]
	tu=idx.parse(hdr,args=args,options=0)
	return tu

def kind_is(c,k): return str(c.kind)==k

def dump_clang(tu):
	structs={}; enums={}; typedefs={}; funcs=[]
	for c in tu.cursor.get_children():
		if kind_is(c,"CursorKind.STRUCT_DECL") and c.is_definition() and c.spelling:
			fs=[]
			for f in c.get_children():
				if kind_is(f,"CursorKind.FIELD_DECL"):
					fs.append({"name":f.spelling,"type":f.type.spelling})
			structs[c.spelling]=fs
		elif kind_is(c,"CursorKind.ENUM_DECL") and c.spelling:
			it=[]
			for e in c.get_children():
				if kind_is(e,"CursorKind.ENUM_CONSTANT_DECL"):
					it.append({"name":e.spelling,"value":e.enum_value})
			enums[c.spelling]=it
		elif kind_is(c,"CursorKind.TYPEDEF_DECL") and c.spelling:
			typedefs[c.spelling]=c.underlying_typedef_type.spelling
		elif kind_is(c,"CursorKind.FUNCTION_DECL") and c.spelling:
			if c.type.is_function_variadic(): continue
			ps=[a.type.spelling for a in c.get_arguments()]
			funcs.append({"name":c.spelling,"ret":c.result_type.spelling,"args":ps})
	return structs,enums,typedefs,funcs

def dump_funcs_regex(txt):
	out=[]
	for m in re.finditer(r"\b(?:RLAPI|API|extern)\s+([_A-Za-z][\w\s\*\d]*?)\s+([A-Za-z_]\w*)\s*\(([^;]*?)\)\s*;",txt):
		ret=m.group(1).strip(); name=m.group(2).strip(); args=m.group(3).strip()
		if "..." in args or "(*" in args: continue
		ps=[] if args in ("","void") else [a.strip() for a in args.split(",")]
		out.append({"name":name,"ret":ret,"args":ps})
	return out

def ldconfig_paths():
	if platform.system().lower()!="linux": return {}
	try: out=sh(["ldconfig","-p"])
	except Exception: return {}
	mp={}
	for l in out.splitlines():
		if " => " not in l: continue
		a,b=l.split(" => ",1)
		so=a.strip().split()[0]
		mp[so]=b.strip()
	return mp

def resolve_lib(pkg,header_base):
	sysn=platform.system().lower()
	ext="dll" if sysn=="windows" else ("dylib" if sysn=="darwin" else "so")
	L=[]; lnames=[]
	if pkg:
		for t in pkg_libs(pkg):
			if t.startswith("-L"): L.append(t[2:])
			elif t.startswith("-l"): lnames.append(t[2:])
	if not lnames:
		g=os.path.splitext(header_base)[0]
		lnames=[g,g.replace("_","-"),g.replace("-","_")]
	ldp=ldconfig_paths()
	cands=[]
	for n in lnames:
		if sysn=="windows":
			cands += [f"{n}.{ext}",f"lib{n}.{ext}"]
		else:
			cands += [f"lib{n}.{ext}",f"lib{n}.{ext}.0",f"lib{n}.{ext}.1",f"lib{n}.{ext}.2",f"lib{n}.{ext}.3",f"lib{n}.{ext}.4",f"lib{n}.{ext}.5"]
		for d in L:
			for s in list(cands):
				p=os.path.join(d,s)
				if os.path.isfile(p): return [p]+cands,n
		for s in list(cands):
			if s in ldp: return [ldp[s],s]+cands,n
		f=ctypes.util.find_library(n)
		if f: return [f]+cands,n
	return cands,(lnames[0] if lnames else None)

def ny_call(ret,argc):
	r=ret.strip()
	if r=="void": return f"ffi.call{argc}_void"
	return f"ffi.call{argc}"

def emit_mod(outdir,dlcands,funcs,enums,types):
	os.makedirs(outdir,exist_ok=True)
	ny=[]
	ny.append("use std.os.ffi as ffi\n")
	if enums:
		for en,items in enums.items():
			ny.append(f";; enum {en}")
			for it in items:
				ny.append(f'def {it["name"]} = {it["value"]}')
			ny.append("")
	ny.append(f'def h = ffi.dlopen("{dlcands[0]}",2)')
	for s in dlcands[1:]:
		ny.append(f'if(h==0){{ h = ffi.dlopen("{s}",2) }}')
	ny.append('if(h==0){ print("ffi: lib not found") } else {')
	for f in funcs:
		ny.append(f'\tdef _{f["name"]} = ffi.dlsym(h,"{f["name"]}")')
	ny.append("")
	for f in funcs:
		ny.append(f'\tassert(_{f["name"]}!=0)')
	ny.append("")
	for f in funcs:
		argc=len(f["args"])
		args=",".join(f"a{i}" for i in range(argc))
		call=ny_call(f["ret"],argc)
		tail=(","+args) if argc else ""
		ny.append(f'\tfn {f["name"]}({args}){{ return {call}(_{f["name"]}{tail}) }}')
	ny.append("\n\tfn unload(){ ffi.dlclose(h) }")
	ny.append("}")
	open(os.path.join(outdir,"mod.ny"),"w").write("\n".join(ny))
	open(os.path.join(outdir,"types.json"),"w").write(json.dumps(types,indent=2,sort_keys=True))

def main():
	if len(sys.argv)!=2: raise SystemExit("usage: auto_wrap_c.py <header.h>")
	header=sys.argv[1]
	hdr,pkg,cft=find_header(header)
	if not hdr: raise SystemExit(f"header not found: {header}")
	inc,defs=split_cflags(cft)
	cindex=try_clang()
	structs={}; enums={}; typedefs={}; funcs=[]
	if cindex:
		tu=clang_parse(cindex,hdr,inc,defs)
		structs,enums,typedefs,funcs=dump_clang(tu)
	else:
		txt=open(hdr,"r",errors="ignore").read()
		funcs=dump_funcs_regex(txt)
	base=os.path.basename(hdr)
	dlcands,libname=resolve_lib(pkg,base)
	if not dlcands: raise SystemExit("library not found")
	outdir=os.path.join("gen",(libname or os.path.splitext(base)[0]))
	types={"header":hdr,"pkg":pkg,"structs":structs,"enums":enums,"typedefs":typedefs,"funcs":funcs,"dlopen_candidates":dlcands}
	emit_mod(outdir,dlcands,funcs,enums,types)
	print(os.path.join(outdir,"mod.ny"))

if __name__=="__main__":
	main()
