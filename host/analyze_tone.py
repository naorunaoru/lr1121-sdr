#!/usr/bin/env python3
"""Analyze native CF32 probe output; phase residuals assume a dominant CW."""
import argparse
import json
from pathlib import Path
import numpy as np
from scipy.optimize import minimize_scalar


def fast_tone(z):
    z=np.asarray(z,dtype=complex);n=np.arange(len(z));energy=float(np.vdot(z-z.mean(),z-z.mean()).real)
    if not energy:raise ValueError('Constant samples')
    fftn=1<<(len(z)*8-1).bit_length()
    spectrum=np.fft.fft((z-z.mean())*np.hanning(len(z)),fftn)
    f=float(np.fft.fftfreq(fftn)[np.argmax(abs(spectrum))])
    def solve(f):
        e=np.exp(2j*np.pi*f*n)
        matrix=np.array([[len(z),e.sum()],[e.sum().conjugate(),len(z)]])
        dc,a=np.linalg.solve(matrix,np.array([z.sum(),np.vdot(e,z)]))
        residual=z-dc-a*e
        return float(np.vdot(residual,residual).real),dc,a
    opt=minimize_scalar(lambda f:solve(f)[0],bounds=(f-1/fftn,f+1/fftn),method='bounded',options={'xatol':1e-14})
    error,dc,a=solve(float(opt.x))
    return dict(cycles_per_pair=float(opt.x),fraction_of_ac_power=1-error/energy,dc_counts=[float(dc.real),float(dc.imag)],amplitude_counts=float(abs(a)))


def phase_boundaries(z,block=125):
    if len(z)%block or len(z)<block*2:raise ValueError('Need complete multiple blocks')
    dc=z.mean();phase=np.unwrap(np.angle(z-dc)).reshape(-1,block)
    x=np.arange(block)-(block-1)/2
    slopes=phase@x/np.dot(x,x)
    centers=phase.mean(axis=1)
    jumps=np.diff(centers)-block*(slopes[:-1]+slopes[1:])/2
    equivalent=jumps/((slopes[:-1]+slopes[1:])/2)
    residual=phase-(centers[:,None]+slopes[:,None]*x)
    return dict(block_samples=block,phase_slopes=slopes.tolist(),jumps_rad=jumps.tolist(),
                equivalent_extra_pairs=equivalent.tolist(),phase_rms_rad=np.std(residual,axis=1).tolist(),
                max_abs_boundary_pairs=float(np.max(abs(equivalent))),
                boundary_pairs_rms=float(np.sqrt(np.mean(equivalent**2))),
                maximum_block_phase_rms_rad=float(np.max(np.std(residual,axis=1))))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    parser.add_argument('--rate', type=int, default=500000)
    args = parser.parse_args()
    z = np.fromfile(args.capture, dtype='<c8')
    if len(z) < 4500 or len(z) % 125:
        parser.error('Need at least 4500 samples, in complete 125-sample banks')
    fit = fast_tone(z[500:4500])
    boundaries = phase_boundaries(z)
    print(json.dumps(dict(
        samples=len(z), fit=fit,
        offset_hz=fit['cycles_per_pair'] * args.rate,
        max_boundary_samples=boundaries['max_abs_boundary_pairs'],
        boundary_outliers=int(np.sum(np.abs(boundaries['equivalent_extra_pairs']) > .5)),
        min_slope=min(boundaries['phase_slopes']),
        max_slope=max(boundaries['phase_slopes']),
    ), indent=2))


if __name__ == '__main__':
    main()
