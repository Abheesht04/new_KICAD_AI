"""pcba CLI contract: help, usage errors, colour/JSON hygiene, exit-code propagation,
paths with spaces and shell metacharacters, legacy compatibility and fail-closed verification.

Usage: test_pcba_cli.py PCBA HWC COPPER_CORE [unittest args]
Exit status: 0 all ran and passed, 1 failure, 77 something was skipped.
"""
import json, os, re, subprocess, sys, tempfile, unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
PCBA,HWC,CORE=(str(Path(sys.argv.pop(1)).resolve()) for _ in range(3))
sys.path.insert(0,str(ROOT/'tools'))
import copper
from hwc_kicad import one, direct, Atom as A
FIX_SYM=ROOT/'examples/generated/divider/libraries/symbols'
FIX_FP=ROOT/'examples/generated/divider/libraries/footprints'
ESC='\x1b'
STABLE={'help','version','doctor','test','project new','project check','project build','project verify','project fabricate',
        'simulate dc','simulate ac','simulate transient','parts search','parts add','parts remove','library scan','dfm check'}
PLANNED={'parts import','place','route'}
EXIT={'PASS':0,'FAIL':1,'INVALID_INPUT':2,'UNVERIFIED':3,'NOT_IMPLEMENTED':4,'USAGE':64,'ERROR':70}

def clean_env(**extra):
    env={k:v for k,v in os.environ.items() if k not in ('NO_COLOR','PCBA_FORCE_COLOR','PCBA_PYTHON','PCBA_ROOT','TERM')}
    env.update(extra);return env
def run(*args,exe=PCBA,env=None,cwd=None):
    return subprocess.run([exe,*map(str,args)],capture_output=True,text=True,encoding='utf-8',errors='replace',env=env or clean_env(),cwd=cwd,timeout=300)
def envelope(test,r):
    """stdout must be exactly one JSON document with no colour or prose, consistent with the exit code."""
    test.assertNotIn(ESC,r.stdout)
    doc=json.loads(r.stdout)
    test.assertEqual(set(doc),{'schema','tool','version','command','status','exit_code','message','result'})
    test.assertEqual(doc['exit_code'],r.returncode,doc);test.assertEqual(EXIT[doc['status']],r.returncode,doc)
    return doc
def tree_with(name='divider',rated=False,**changes):
    t=copper.load(ROOT/f'examples/v2/{name}.copper')
    if rated:  # synthetic, explicitly supplied test ratings; not data for a real part
        for p in direct(one(t,'parts'),'part'):
            if p[1]=='resistor':p.extend([copper.sx('power_w',A('0.1')),copper.sx('voltage_v',A('50')),copper.sx('height_mm',A('0.55'))])
            if p[1]=='header3':p.append(copper.sx('height_mm',A('2.5')))
    for ref,value in changes.items():one(next(c for c in direct(one(t,'components'),'component') if c[1]==ref),'value')[1]=A(value)
    return t

class Base(unittest.TestCase):
    def setUp(self):self.tmp=tempfile.TemporaryDirectory();self.dir=Path(self.tmp.name)
    def tearDown(self):self.tmp.cleanup()
    def write(self,tree,name='p.copper'):path=self.dir/name;copper.write_tree(path,tree);return path

class Help(Base):
    def test_help_lists_every_command_and_exit_code(self):
        r=run('--help');self.assertEqual(r.returncode,0,r.stderr)
        commands=json.loads(run('help','--json').stdout)['result']['commands']
        self.assertEqual({c['command'] for c in commands if c['stability']=='stable'},STABLE)
        self.assertEqual({c['command'] for c in commands if c['stability']=='NOT_IMPLEMENTED'},PLANNED)
        for c in commands:self.assertIn(c['usage'],r.stdout);self.assertIn(c['summary'],r.stdout)
        for status,code in EXIT.items():self.assertRegex(r.stdout,rf'\b{code}\s+{status}\b')
        self.assertIn('Planned, NOT_IMPLEMENTED',r.stdout)
    def test_every_command_has_help_with_examples(self):
        for command in STABLE|PLANNED:
            words=command.split();r=run('help',*words);self.assertEqual(r.returncode,0,command)
            for heading in ('Usage:','Examples:','Exit codes:'):self.assertIn(heading,r.stdout,command)
            self.assertEqual(run(*words,'--help').stdout,r.stdout,command)
            if command in PLANNED:self.assertIn('NOT_IMPLEMENTED',r.stdout)
    def test_group_help_and_bare_group(self):
        r=run('project','--help');self.assertEqual(r.returncode,0);self.assertIn('pcba project build',r.stdout);self.assertNotIn('simulate dc',r.stdout)
        r=run('project');self.assertEqual(r.returncode,64);self.assertIn('needs a subcommand',r.stderr);self.assertEqual(r.stdout,'')
    def test_version(self):
        r=run('version');self.assertEqual(r.returncode,0);self.assertIn('pcba 2.0.0-dev',r.stdout);self.assertIn('C17',r.stdout)
        self.assertEqual(run('--version').stdout,r.stdout)
        doc=envelope(self,run('version','--json'));self.assertEqual(doc['result']['kicad_target'],'9.x');self.assertEqual(doc['result']['c_standard'],'C17')

class Usage(Base):
    def test_invalid_and_missing_arguments_exit_64(self):
        f=str(ROOT/'examples/v2/divider.copper');rc=str(ROOT/'examples/v2/rc.copper')
        cases=[['frobnicate'],['project','frob'],['project','check'],['project','check',f,f],['project','check',f,'--bogus'],
               ['project','check',f,'-x'],['project','build',f],['project','build',f,'--out'],['project','build',f,'--out',''],
               ['project','build',f,'--out','a','--out','b'],['project','build',f,'--out','a','--draft=yes'],['project','verify'],
               ['project','new','resistor','--out','x.copper'],['project','new','divider','--out','x.copper','--vin','nan'],
               ['project','new','divider','--out','x.copper','--r','10k'],['simulate','dc'],
               ['simulate','transient',rc,'--steps','10'],['simulate','transient',rc,'--dt','1e-4']]
        cases+=[['simulate','transient',rc,'--dt',dt,'--steps',steps] for dt,steps in
                [('abc','10'),('-1','10'),('0','10'),('nan','10'),('inf','10'),('1001','10'),('1e-4','0'),('1e-4','1.5'),('1e-4','100001'),('1e-4','-3')]]
        cases+=[['parts','search'],['parts','add','--catalogue','c.parts','--id','x','--kind','X'],
                ['parts','add','--catalogue','c.parts','--id','x','--kind','R','--power','-1'],['parts','add','--id','x','--kind','R'],
                ['parts','add','--catalogue','c.parts','--id','x','--kind','R','--power','0'],['parts','remove','--id','x'],
                ['simulate','ac',rc],['simulate','ac',rc,'--start','1','--stop','10'],
                ['simulate','ac',rc,'--start','0','--stop','10','--points','3'],['simulate','ac',rc,'--start','10','--stop','1','--points','3'],
                ['simulate','ac',rc,'--start','1','--stop','10','--points','0'],['simulate','ac',rc,'--start','1','--stop','10','--points','1'],
                ['simulate','ac',rc,'--start','nan','--stop','10','--points','3','--sweep','lin'],['simulate','ac',rc,'--start','1','--stop','10','--points','3','--sweep','cubic'],
                ['dfm','check'],['library','scan','extra'],['doctor','extra'],['test','--quick','--full'],['test','--bogus'],
                ['version','extra'],['help','no-such-command']]
        for argv in cases:
            r=run(*argv,cwd=self.dir);self.assertEqual(r.returncode,64,f'{argv}: {r.stdout}{r.stderr}')
            self.assertEqual(r.stdout,'',argv);self.assertIn('error',r.stderr,argv)
            doc=envelope(self,run(*argv,'--json',cwd=self.dir));self.assertEqual(doc['status'],'USAGE',argv);self.assertTrue(doc['message'],argv)
        self.assertEqual(list(self.dir.iterdir()),[],'usage errors must not create files')
    def test_missing_inputs_are_invalid_input(self):
        for argv in [['project','check','nope.copper'],['simulate','dc','nope.copper'],['dfm','check','nope'],['project','verify','--out','no_build_dir'],
                     ['project','build','nope.copper','--out','o'],['parts','search','r','--catalogue','nope.parts'],['parts','remove','--catalogue','nope.parts','--id','x']]:
            r=run(*argv,cwd=self.dir);self.assertEqual(r.returncode,2,argv);self.assertIn('not found',r.stdout)
            self.assertEqual(envelope(self,run(*argv,'--json',cwd=self.dir))['status'],'INVALID_INPUT')
    def test_planned_commands_are_not_implemented(self):
        f=ROOT/'examples/v2/rc.copper'
        for argv in [['place',f],['route',f],['parts','import',f]]:
            r=run(*argv);self.assertEqual(r.returncode,4,argv);self.assertIn('NOT_IMPLEMENTED',r.stdout)
            self.assertEqual(envelope(self,run(*argv,'--json'))['status'],'NOT_IMPLEMENTED')
        self.assertEqual(run('place').returncode,64)  # interface is still validated

class Output(Base):
    def test_colour_policy(self):
        self.assertNotIn(ESC,run('--help').stdout,'redirected output must be plain')
        forced=clean_env(PCBA_FORCE_COLOR='1')
        self.assertIn(ESC,run('--help',env=forced).stdout)
        self.assertNotIn(ESC,run('--help',env=dict(forced,NO_COLOR='1')).stdout)
        self.assertNotIn(ESC,run('--help','--no-color',env=forced).stdout)
        self.assertNotIn(ESC,run('help','--json',env=forced).stdout)
        r=run('project','check',ROOT/'examples/v2/divider.copper',env=forced);self.assertIn('UNVERIFIED',r.stdout)
        self.assertIn('UNVERIFIED project check',re.sub(r'\x1b\[[0-9;]*m','',r.stdout),'status must be readable without colour')
    def test_json_mode_is_one_clean_document(self):
        forced=clean_env(PCBA_FORCE_COLOR='1');f=ROOT/'examples/v2/divider.copper'
        cases=[['version'],['help'],['doctor','--out',self.dir/'o'],['project','check',f],['simulate','dc',f],['dfm','check',f],
               ['parts','search','resistor'],['simulate','transient',ROOT/'examples/v2/rc.copper','--dt','1e-4','--steps','5'],
               ['simulate','ac',ROOT/'examples/v2/rc_ac.copper','--start','1','--stop','100','--points','3'],
               ['project','check',self.dir/'missing.copper'],['project','frob'],['place',f]]
        for argv in cases:
            r=run(*argv,'--json',env=forced);doc=envelope(self,r)
            self.assertEqual(doc['tool'],'pcba');self.assertEqual(doc['version'],'2.0.0-dev')
            self.assertNotIn(ESC,r.stderr,argv)

class ExitCodes(Base):
    def test_analysis_statuses_propagate(self):
        f=ROOT/'examples/v2/divider.copper'
        self.assertEqual(run('project','check',f).returncode,3)            # unknown ratings, no geometry: UNVERIFIED
        self.assertEqual(run('simulate','dc',f).returncode,0)              # the DC solve itself passes
        t=tree_with();one(t,'targets')[1][2]=A('4');one(t,'targets')[1][3]=A('4.1')
        self.assertEqual(run('project','check',self.write(t)).returncode,1)  # target violated: FAIL
        t=tree_with();one(direct(one(t,'parts'),'part')[0],'kind')[1]=A('U')
        doc=envelope(self,run('simulate','dc',self.write(t),'--json'));self.assertEqual(doc['status'],'UNVERIFIED')
        t=tree_with();one(one(one(t,'components'),'component'),'pins')[1][1]=A('MISSING')
        self.assertEqual(run('project','check',self.write(t)).returncode,2)  # contract violation: INVALID_INPUT
        t=tree_with();one(t,'nets').append(A('FLOAT'))
        self.assertEqual(run('simulate','dc',self.write(t)).returncode,1)   # singular system: FAIL
    def test_ac(self):
        ac=ROOT/'examples/v2/rc_ac.copper';out=self.dir/'ac.csv'
        r=run('simulate','ac',ac,'--start','1','--stop','1e4','--points','5','--out',out);self.assertEqual(r.returncode,0,r.stderr)
        self.assertIn('|V(OUT)|',r.stdout);self.assertTrue(out.read_text().startswith('frequency_hz,status,V(VIN)_re'))
        doc=envelope(self,run('simulate','ac',ac,'--start','15.9154943091895','--stop','15.9154943091895','--points','1','--json'))
        row=doc['result']['data']['rows'][0];self.assertAlmostEqual(row['V(OUT)_mag'],2**-0.5,places=9);self.assertAlmostEqual(row['V(OUT)_phase_deg'],-45,places=6)
        self.assertEqual(run('simulate','ac',ROOT/'examples/v2/rc.copper','--start','1','--stop','10','--points','2').returncode,2)  # no AC excitation
        t=tree_with('rc');one(direct(one(t,'parts'),'part')[0],'kind')[1]=A('U');v=next(c for c in direct(one(t,'components'),'component') if c[1]=='V1')
        v.append(copper.sx('ac',A('1'),A('0')))
        self.assertEqual(envelope(self,run('simulate','ac',self.write(t),'--start','1','--stop','10','--points','2','--json'))['status'],'UNVERIFIED')
    def test_transient(self):
        rc=ROOT/'examples/v2/rc.copper'
        r=run('simulate','transient',rc,'--dt','1e-4','--steps','100');self.assertEqual(r.returncode,0,r.stderr);self.assertTrue(r.stdout.startswith('time_s,'))
        doc=envelope(self,run('simulate','transient',rc,'--dt','1e-4','--steps','100','--json'))
        self.assertAlmostEqual(doc['result']['data']['final']['OUT_V'],5*(1-(1/1.01)**100),places=8);self.assertEqual(len(doc['result']['data']['rows']),100)
        out=self.dir/'w.csv';self.assertEqual(run('simulate','transient',rc,'--dt','1e-4','--steps','10','--out',out).returncode,0);self.assertTrue(out.is_file())
        t=tree_with('rc');one(direct(one(t,'parts'),'part')[1],'kind')[1]=A('U')
        self.assertEqual(run('simulate','transient',self.write(t),'--dt','1e-4','--steps','10').returncode,3)
    def test_failed_or_incomplete_verification_never_succeeds(self):
        src=self.write(tree_with(rated=True),'rated.copper');out=self.dir/'build';nokicad=str(self.dir/'no-kicad-cli')
        doc=envelope(self,run('project','build',src,'--out',out,'--symbols',FIX_SYM,'--footprints',FIX_FP,'--json'))
        self.assertEqual(doc['status'],'PASS');self.assertEqual(doc['result']['data']['native_verification'],'NOT_RUN')
        self.assertEqual(run('dfm','check',out).returncode,0)
        doc=envelope(self,run('project','verify','--out',out,'--kicad-cli',nokicad,'--json'))
        self.assertEqual(doc['status'],'UNVERIFIED');self.assertEqual(doc['result']['data']['verification']['gates'][0]['status'],'UNVERIFIED')
        r=run('project','fabricate','--out',out,'--kicad-cli',nokicad);self.assertEqual(r.returncode,3);self.assertFalse((out/'fabrication').exists())
        copper.write_tree(src,tree_with(rated=True,R2='22000'))
        self.assertEqual(run('project','verify','--out',out,'--kicad-cli',nokicad).returncode,1)  # stale source: FAIL
        copper.write_tree(src,tree_with(rated=True))
        with (out/'design.kicad_pcb').open('a') as f:f.write('\n')
        self.assertEqual(run('project','verify','--out',out,'--kicad-cli',nokicad).returncode,1)  # tampered output: FAIL
        generic=self.write(tree_with(),'generic.copper')
        self.assertEqual(run('project','build',generic,'--out',self.dir/'g','--symbols',FIX_SYM,'--footprints',FIX_FP).returncode,3)
        self.assertEqual(run('dfm','check',generic).returncode,3)  # no geometry: UNVERIFIED, never PASS
        t=tree_with(rated=True);one(t,'targets')[1][2]=A('4');one(t,'targets')[1][3]=A('4.1')
        self.assertEqual(run('project','build',self.write(t,'bad.copper'),'--out',self.dir/'d','--symbols',FIX_SYM,'--footprints',FIX_FP).returncode,1)
        self.assertEqual(run('project','build',self.dir/'bad.copper','--out',self.dir/'d','--draft','--symbols',FIX_SYM,'--footprints',FIX_FP).returncode,1)
    def test_parts_add_and_remove(self):
        cat=self.dir/'mine.parts'
        doc=envelope(self,run('parts','add','--catalogue',cat,'--id','d1','--kind','D','--model','shockley.diode','--pinmap','cathode=1,anode=2',
                              '--symbol','Device:D','--footprint','Diode_SMD:D_SOD-123','--provenance','synthetic test entry','--json'))
        self.assertEqual(doc['status'],'PASS');saved=cat.read_bytes()
        r=run('parts','add','--catalogue',cat,'--id','d2','--kind','D','--pinmap','cathode=1','--symbol','Device:D','--footprint','Diode_SMD:D_SOD-123')
        self.assertEqual(r.returncode,2);self.assertEqual(cat.read_bytes(),saved)  # rejected addition leaves the file untouched
        hit=envelope(self,run('parts','search','d1','--catalogue',cat,'--json'))['result']['data']['parts'][0]
        self.assertEqual((hit['model'],hit['pinmap'],hit['provenance'],hit['catalogue']),('shockley.diode',{'cathode':'1','anode':'2'},'synthetic test entry','user:mine.parts'))
        q=['parts','add','--catalogue',cat,'--id','q1','--kind','Q','--symbol','Transistor_BJT:BC547','--footprint','Package_TO_SOT_THT:TO-92_Inline']
        r=run(*q);self.assertEqual(r.returncode,2);self.assertIn('explicit (model',r.stderr)  # NPN/PNP is never inferred
        self.assertEqual(run(*q,'--model','ebers_moll.npn','--pinmap','collector=1,base=2,emitter=3').returncode,0)
        self.assertEqual(run('parts','remove','--catalogue',cat,'--id','d1').returncode,0)
        self.assertEqual(run('parts','remove','--catalogue',cat,'--id','d1').returncode,2)
    def fake_backend(self,body):
        root=self.dir/'fake';(root/'tools').mkdir(parents=True,exist_ok=True)
        (root/'tools/copper.py').write_text('import sys\nresult=sys.argv[sys.argv.index("--result-json")+1]\n'+body)
        return clean_env(PCBA_ROOT=str(root))
    def test_backend_failures_cannot_become_success(self):
        f=ROOT/'examples/v2/divider.copper'
        env=self.fake_backend('raise SystemExit(0)\n')  # exits 0 but reports nothing
        self.assertEqual(run('project','check',f,env=env).returncode,70)
        env=self.fake_backend('open(result,"w").write(\'{"status": "PASS", "message": "", "data": null}\')\nraise SystemExit(2)\n')
        doc=envelope(self,run('project','check',f,'--json',env=env));self.assertEqual(doc['status'],'ERROR')  # PASS with nonzero exit
        env=self.fake_backend('open(result,"w").write(\'{"status": "FAIL", "message": "x", "data": null}\')\nraise SystemExit(0)\n')
        self.assertEqual(run('project','check',f,env=env).returncode,1)
        env=self.fake_backend('open(result,"w").write("not json")\n')
        self.assertEqual(run('project','check',f,env=env).returncode,70)
        r=run('project','check',f,env=clean_env(PCBA_PYTHON=str(self.dir/'no-python')))
        self.assertEqual(r.returncode,3);self.assertIn('Python',r.stdout)

class Paths(Base):
    NAMES=['dir with spaces','a&b;c','%PATH%','(1) [x]',"it's",'ünïcode']
    def test_paths_with_spaces_and_metacharacters(self):
        for name in self.NAMES:
            folder=self.dir/name/'inner dir';folder.mkdir(parents=True);path=folder/f'{name}.copper'
            r=run('project','new','divider','--out',path);self.assertEqual(r.returncode,0,f'{name}: {r.stderr}');self.assertTrue(path.is_file(),name)
            doc=envelope(self,run('project','check',path,'--json'));self.assertEqual(Path(doc['result']['data']['project']),path.resolve(),name)
            r=run('project','check',path,exe=HWC,cwd=self.dir);self.assertEqual(r.returncode,0,f'hwc {name}: {r.stderr}');json.loads(r.stdout)
    def test_arguments_reach_backend_verbatim(self):
        query='he said "R" & 100% ;$(x) \\ end\\'
        doc=envelope(self,run('parts','search',query,'--json'));self.assertEqual(doc['result']['data']['query'],query)
    def test_no_shell_in_launchers(self):
        forbidden=re.compile(r'\bsystem\s*\(|\bpopen\s*\(|_spawn|ShellExecute|shell\s*=\s*True|os\.system|os\.popen')
        files=[*ROOT.glob('apps/**/*.c'),*ROOT.glob('desktop/*.cpp'),*ROOT.glob('tools/*.py')]
        self.assertTrue(files)
        for path in files:
            for no,line in enumerate(path.read_text(encoding='utf-8',errors='replace').splitlines(),1):
                self.assertIsNone(forbidden.search(line),f'{path.relative_to(ROOT)}:{no}: {line.strip()}')

class Legacy(Base):
    def test_hwc_and_core_still_work(self):
        r=run('--help',exe=HWC,cwd=self.dir);self.assertEqual(r.returncode,0);self.assertIn('pcba --help',r.stdout)
        self.assertFalse((self.dir/'out').exists(),'hwc --help must not create out/')
        r=run('project','check',ROOT/'examples/v2/divider.copper',exe=HWC,cwd=self.dir);self.assertEqual(r.returncode,0,r.stderr);self.assertTrue(json.loads(r.stdout)['dc_solved'])
        r=run('analyze',ROOT/'examples/v2/divider.copper',exe=CORE);self.assertEqual(r.returncode,0);json.loads(r.stdout)

class Doctor(Base):
    def test_doctor_reports_every_component(self):
        doc=envelope(self,run('doctor','--out',self.dir/'o','--json'))
        runtime={c['check']:c['status'] for c in doc['result']['runtime']}
        self.assertEqual({k:runtime[k] for k in ('pcba','source_root','python_launcher','copper_core')},dict.fromkeys(('pcba','source_root','python_launcher','copper_core'),'PASS'))
        pipeline={c['check']:c['status'] for c in doc['result']['pipeline']['data']['checks']}
        for name in ('python','copper_core','output_dir','kicad_cli','symbol_libraries','footprint_libraries','vulkan_runtime','desktop_app'):self.assertIn(name,pipeline)
        self.assertEqual(pipeline['output_dir'],'PASS')
        self.assertEqual(doc['status'],'PASS' if all(pipeline[k]=='PASS' for k in ('kicad_cli','symbol_libraries','footprint_libraries')) else 'UNVERIFIED')
    def test_doctor_without_kicad_or_python(self):
        doc=envelope(self,run('doctor','--out',self.dir/'o','--kicad-cli',self.dir/'none','--json'))
        self.assertEqual(doc['status'],'UNVERIFIED');self.assertIn('9.x',json.dumps(doc['result']))
        r=run('doctor','--out',self.dir/'o',env=clean_env(PCBA_PYTHON=str(self.dir/'no-python')))
        self.assertEqual(r.returncode,1);self.assertIn('PCBA_PYTHON',r.stdout)

if __name__=='__main__':
    result=unittest.main(verbosity=2,exit=False).result
    sys.exit(1 if not result.wasSuccessful() else 77 if result.skipped else 0)
