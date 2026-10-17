import numpy as np, sys
def bh(n):
    t=2*np.pi*np.arange(n)/(n-1)
    return 0.35875-0.48829*np.cos(t)+0.14128*np.cos(2*t)-0.01168*np.cos(3*t)
x=np.fromfile('/tmp/o.raw',dtype='<i2').reshape(-1,2)[:,0].astype(float)/32768
core=x[2000:-2000]; n=len(core); sp=np.abs(np.fft.rfft(core*bh(n))); k=np.argmax(sp[2:])+2
pw=sp**2; sig=pw[k-6:k+7].sum(); noise=pw[3:].sum()-sig
print('  %-22s freq %8.1f  SNR %5.1f dB  amp %.3f'%(' '.join(sys.argv[1:]), k*48000/n, 10*np.log10(sig/noise), np.abs(core).max()*32768/16000), open('/tmp/e.txt').read().strip().split('\n')[-1])
