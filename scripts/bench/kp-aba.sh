#!/bin/bash
# A-B-A on the Kp stability cap, via rate_filter_lag_us -- the only runtime route to Kp, which is
# pinned AT the cap (0.20 * pi/2L), so lowering the cap is the only way to lower Kp.
#
#   arm A   rate_filter_lag_us 0        L=1.75 s   Kp cap 0.180   (today's default)
#   arm B   rate_filter_lag_us 3000000  L=3.25 s   Kp cap 0.097   (1.86x lower)
#   arm A'  back to 0                              drift control
#
# A' is not padding: the wander's correlation time is 60-120 s, so a single before/after measures
# where the wander happened to be rather than what the gain did.
#
# NO ratewhy_ppm CHANGE this time. The P-term is recoverable from ENGINE (rate - xtal) on every
# decision, which is how it was measured before, so there is no reason to raise log volume on the
# task whose logging path allocates.
#
# CRASH GUARD: a board reboot mid-arm invalidates that arm (re-acquisition dominates the wire and
# the counters reset). Boot times and crash tallies are recorded at both ends of every arm and any
# arm that spans a reboot is printed as CONTAMINATED rather than quietly averaged in.
set -u
R=/Users/william/Documents/Personal/git/snapclient-esphome
H=192.168.1.230
A=192.168.1.161; B=192.168.1.185
ARM=720

set_both() { "$R/scripts/servo-param.py" "$1" "$2" $A $B 2>&1 | sed 's/^/    /'; }

state() {   # byte offsets, unix time, crash tallies, last boots -- one line
  ssh -o ConnectTimeout=20 $H bash -s <<'M'
cd /home/william/Documents/git/esphome-snapcast
so=0; as=0
for f in a b observer; do
  v=$(grep -ac 'stack overflow' $f.log 2>/dev/null | tail -1); so=$(( so + ${v:-0} ))
  v=$(grep -ac 'assert failed' $f.log 2>/dev/null | tail -1); as=$(( as + ${v:-0} ))
done
ba=$(grep -a 'rst:0x' a.log | tr -d '\0' | tail -1 | grep -oE '^\[[0-9:]+' | tr -d '[')
bb=$(grep -a 'rst:0x' b.log | tr -d '\0' | tail -1 | grep -oE '^\[[0-9:]+' | tr -d '[')
echo "$(stat -c %s a.log) $(stat -c %s b.log) $(date +%s) $so $as ${ba:-none} ${bb:-none}"
M
}

grade() {   # label a_off b_off since
  ssh -o ConnectTimeout=20 $H bash -s <<M
cd /home/william/Documents/git/esphome-snapcast
python3 scripts/bench/ab-window.py "$1" "$2" "$3" "$4" 2>&1 | grep -vE "last 6|longest|descending"
M
}

pterm() {   # a_off b_off -- P = rate - xtal, per board, over the arm
  ssh -o ConnectTimeout=20 $H bash -s <<M
cd /home/william/Documents/git/esphome-snapcast
for f in a b; do
  off=\$( [ \$f = a ] && echo $1 || echo $2 )
  tail -c +\$off \$f.log | tr -d '\0' | sed -E 's/\x1b\[[0-9;]*m//g' | grep -a ": ENGINE " \
  | python3 -c '
import sys,re,statistics as st
R=[];X=[]
for l in sys.stdin:
    d=dict(re.findall(r"(\w+)=([-+]?[0-9]+\.?[0-9]*)", l.split(": ENGINE ",1)[1]))
    if "rate" in d and "xtal" in d:
        R.append(float(d["rate"])); X.append(float(d["xtal"]))
if len(R)>20:
    p=[r-x for r,x in zip(R,X)]
    print(f"    board '\$f': P sd={st.pstdev(p):.3f} p2p={max(p)-min(p):.2f} ppm  "
          f"rate sd={st.pstdev(R):.3f}  xtal sd={st.pstdev(X):.3f}  n={len(p)}")
else:
    print(f"    board '\$f': only {len(R)} ENGINE samples")
'
done
M
}

run_arm() {  # label value
  echo; echo "### $1   rate_filter_lag_us=$2   $(date +%H:%M:%S)"
  set_both rate_filter_lag_us "$2"
  sleep 30
  read a0 b0 t0 so0 as0 ba0 bb0 <<< "$(state)"
  sleep $ARM
  read a1 b1 t1 so1 as1 ba1 bb1 <<< "$(state)"
  if [ "$ba0" != "$ba1" ] || [ "$bb0" != "$bb1" ] || [ "$so1" -gt "$so0" ] || [ "$as1" -gt "$as0" ]; then
    echo "  *** CONTAMINATED: a board rebooted during this arm"
    echo "      boots a $ba0 -> $ba1   b $bb0 -> $bb1   overflow $so0 -> $so1   assert $as0 -> $as1"
  fi
  grade "$1" "$a0" "$b0" "$t0"
  pterm "$a0" "$b0"
}

echo "### baseline check: uptime clean, no crashes since the flash?"
read a0 b0 t0 so0 as0 ba0 bb0 <<< "$(state)"
echo "    crash tallies: overflow=$so0 assert=$as0   last boots a=$ba0 b=$bb0"

run_arm "ARM A  Kp cap 0.180 (baseline)"        0
run_arm "ARM B  Kp cap 0.097 (1.86x lower)"     3000000
run_arm "ARM A' Kp cap 0.180 (drift control)"   0

echo; echo "### restored to default; done $(date +%H:%M:%S)"
