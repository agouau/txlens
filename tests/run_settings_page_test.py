"""Run the embedded settings page in headless Edge with a deterministic API."""
from pathlib import Path
import argparse
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--no-sandbox", action="store_true",
                    help="Use only for this local mock fixture when nested browser sandboxes are unavailable")
args = parser.parse_args()
browser = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft/Edge/Application/msedge.exe"
MOCK = r'''
<script>
const calls=[];
let passwordAccepted=true;
let retryOnce=false,lastRequestAt=-Infinity,rateLimited=0;
window.fetch=async (url,options={})=>{
    const now=performance.now();
    calls.push({url,...options,at:now});
    if(retryOnce||now-lastRequestAt<499){
        retryOnce=false;++rateLimited;
        return {ok:false,status:429,text:async()=>'Please wait half a second before trying again.'};
    }
    lastRequestAt=now;
    if(!passwordAccepted||options.headers?.['X-Setup-Password']!=='123456')
        return {ok:false,status:403,text:async()=>'Incorrect setup code'};
    return {ok:true,json:async()=>options.method==='POST'?{message:'Saved'}:{
        sta_ssid:'Upstream',ap_ssid:'Device',sta_has_password:true,saved:true,
        state:'Ready',ap_ip:'192.168.50.1',sta_ip:'192.168.1.20',abuseipdb_configured:false}};
};
</script>
'''
TEST = r'''
<script>
(async()=>{
    const check=(value,message)=>{if(!value)throw Error(message);};
    await new Promise(resolve=>setTimeout(resolve,0));
    const qrCase=location.search.includes('qr');
    if(qrCase){
        check(calls.length===1&&unlocked,'QR should unlock automatically');
        check(!location.hash,'QR secret should be removed from URL');
        check($('setup_password').value==='123456','QR should fill password');
    }else{
        check(calls.length===0&&$('fields').disabled,'No API access before password entry');
        check($('configuration').hidden&&$('settings').getClientRects().length===0,'Settings must be hidden before authentication');
        check(!$('pairing').hidden,'Password form must be visible');
        $('setup_password').value='000000';
        await $('pairing').onsubmit({preventDefault(){}});
        check(!unlocked&&$('fields').disabled&&$('abuse_fields').disabled,'Wrong password must keep forms locked');
        check($('configuration').hidden&&$('settings').getClientRects().length===0,'Wrong password must keep settings hidden');
        $('setup_password').value='123456';
        await $('pairing').onsubmit({preventDefault(){}});
        check(unlocked&&!$('fields').disabled&&!$('abuse_fields').disabled,'Manual password should unlock');
    }
    check(!$('configuration').hidden&&$('pairing').hidden,'Successful authentication should reveal only configuration');
    retryOnce=true;
    await load();
    check(rateLimited===1&&unlocked&&setupPassword==='123456','429 should retry without locking or clearing the code');
    $('abuse_key').value='test-api-key';
    await $('abuse_settings').onsubmit({preventDefault(){}});
    check(calls.some(c=>c.url==='/api/abuseipdb'&&c.method==='POST'&&c.headers['X-Setup-Password']==='123456'),'API key save must authenticate');
    passwordAccepted=false; // A device restart invalidates the previous password.
    await load();
    check(!unlocked&&$('configuration').hidden&&!$('pairing').hidden,'Rejected password must hide configuration again');
    check(!setupPassword&&!$('setup_password').value,'Rejected password must be cleared');
    passwordAccepted=true;
    $('setup_password').value='123456';
    await $('pairing').onsubmit({preventDefault(){}});
    await $('settings').onsubmit({preventDefault(){}});
    check(calls.some(c=>c.url==='/api/settings'&&c.method==='POST'&&c.headers['X-Setup-Password']==='123456'),'Wi-Fi save must authenticate');
    check(restarting&&$('fields').disabled,'Save should finish in restart state');
    document.body.dataset.testResult='PASS';
})().catch(error=>{document.body.dataset.testResult='FAIL';document.body.dataset.testError=error.message;});
</script>
'''
with tempfile.TemporaryDirectory(prefix="settings-page-") as directory:
    work = Path(directory)
    html = (root / "main/settings.html").read_text(encoding="utf-8")
    # The served production CSP allows these inline scripts. File URLs have no response CSP.
    html = html.replace("<script>", MOCK + "<script>", 1) + TEST
    page = work / "settings.html"
    page.write_text(html, encoding="utf-8")
    for case in ("manual", "qr"):
        url = page.as_uri() + ("?qr#password=123456" if case == "qr" else "")
        result = subprocess.run([
            str(browser), "--headless", "--disable-gpu", "--no-first-run", "--no-default-browser-check",
            f"--user-data-dir={work / ('profile-' + case)}", "--disable-extensions",
            *(["--no-sandbox", "--in-process-gpu"] if args.no_sandbox else []),
            "--dump-dom", "--virtual-time-budget=12000", url,
        ], capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=30)
        if result.returncode or 'data-test-result="PASS"' not in result.stdout:
            raise AssertionError(f"{case} failed: {result.stderr[-1000:]}\n{result.stdout[-2500:]}")
    print("PASS: six-digit password, hidden settings before authentication/after rejection, QR auto-fill, authenticated saves")
