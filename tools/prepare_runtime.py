#!/usr/bin/env python3
"""Install the pinned simulator runtime on current GitHub-hosted macOS."""
import json,subprocess

def run(*args):
 print('+',' '.join(args),flush=True)
 return subprocess.run(args,check=True,text=True,capture_output=False)
def available():
 d=json.loads(subprocess.check_output(['xcrun','simctl','list','runtimes','-j'],text=True))
 return next((r for r in d['runtimes'] if r.get('isAvailable') and r.get('version')=='17.5' and '.iOS-' in r['identifier']),None)
r=available()
if not r:
 run('xcodebuild','-downloadPlatform','iOS','-buildVersion','17.5')
 r=available()
if not r:raise SystemExit('iOS 17.5 runtime is unavailable after official download')
d=json.loads(subprocess.check_output(['xcrun','simctl','list','devices','-j'],text=True))
if not any(x.get('isAvailable') for x in d.get('devices',{}).get(r['identifier'],[])):
 run('xcrun','simctl','create','PV2 iPhone 15 Pro Max','com.apple.CoreSimulator.SimDeviceType.iPhone-15-Pro-Max',r['identifier'])
print('Ready:',r['identifier'],flush=True)
