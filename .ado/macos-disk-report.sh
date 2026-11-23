#!/bin/bash
# Temporary, best-effort disk diagnostics. Parent/child du totals overlap.
exec 2>/dev/null
export LC_ALL=C
label=${2:-${1:-inventory}}
samples=${AGENT_TEMPDIRECTORY:-${TMPDIR:-/tmp}}/disk-samples.tsv
pidfile=$samples.pid
marker=$samples.marker
out=${BUILD_SOURCESDIRECTORY:+$BUILD_SOURCESDIRECTORY/deps/nodejs/out}

emit() { printf 'DISK|%s|%s|%s\n' "$label" "$1" "$2"; }
now() { date +%s; }
utc() { date -u -r "$1" '+%Y-%m-%dT%H:%M:%SZ' || printf 'epoch:%s\n' "$1"; }

# Bound individual probes even on images without GNU timeout.
bounded() {
    local seconds=$1 remaining
    shift
    if [ -n "$probe_deadline" ]; then
        remaining=$((probe_deadline-$(now)))
        [ "$remaining" -gt 0 ] || return 124
        [ "$seconds" -le "$remaining" ] || seconds=$remaining
    fi
    if command -v perl >/dev/null; then
        perl -e '
            my $seconds = shift;
            my $pid = fork;
            defined $pid or exit 127;
            if (!$pid) {
                setpgrp(0, 0) or exit 127;
                exec @ARGV;
                exit 127;
            }
            my $status;
            my $done = eval {
                local $SIG{ALRM} = sub { die "timeout\n" };
                alarm $seconds;
                waitpid($pid, 0);
                $status = $?;
                alarm 0;
                1;
            };
            if (!$done) {
                kill "TERM", -$pid;
                select undef, undef, undef, 1;
                kill "KILL", -$pid;
                waitpid($pid, 0);
                exit 124;
            }
            kill "KILL", -$pid;
            exit(($status & 127) ? 128 + ($status & 127) : $status >> 8);
        ' "$seconds" "$@"
    elif command -v timeout >/dev/null; then
        # Kill the whole group even if its leader would exit on TERM first.
        timeout -k 1 -s KILL "$seconds" "$@"
        local status=$?
        [ "$status" -eq 137 ] && status=124
        return "$status"
    else
        "$@"
    fi
}

usage() {
    local path=$1 seconds=${2:-20} result status remaining
    [ -n "$path" ] && [ -e "$path" ] || return 0
    if [ -n "$probe_deadline" ]; then
        remaining=$((probe_deadline-$(now)))
        if [ "$remaining" -le 0 ]; then emit "du:$path" "status=skipped_budget"; return 0; fi
        [ "$seconds" -le "$remaining" ] || seconds=$remaining
    fi
    result=$(bounded "$seconds" du -sk "$path"); status=$?
    case "$status" in 124|137|142|143) status=timeout ;; esac
    emit "du:$path" "$(printf '%s\n' "$result" | awk 'NR==1 && $1 ~ /^[0-9]+$/ {printf "allocated_KiB=%s GiB=%.2f", $1, $1/1048576}') status=$status"
}

swap() {
    bounded 10 sysctl vm.swapusage | awk '{
        for(i=1;i<=NF;i++) if($i=="used") {
            v=$(i+2); unit=substr(v,length(v)); v+=0
            if(unit=="G") v*=1024; if(unit=="K") v/=1024
            print v; exit
        }
    }'
}

boundary() {
    emit utc "$(utc "$(now)")"
    bounded 10 df -k / | while IFS= read -r line; do emit df-root "$line"; done
    if [ -n "$AGENT_WORKFOLDER" ]; then
        bounded 10 df -k "$AGENT_WORKFOLDER" | while IFS= read -r line; do emit df-work "$line"; done
    fi
    bounded 10 df -Pk / | awk -v l="$label" 'NR==2 && $(NF-4)~/^[0-9]+$/ && $(NF-4)>0 {
        total=$(NF-4); avail=$(NF-2)
        printf "DISK|%s|agent-root|total_KiB=%s available_KiB=%s used_pct=%.2f free_GiB=%.2f\n", l,total,avail,100*(1-avail/total),avail/1048576
    }'
    emit swapusage "$(bounded 10 sysctl vm.swapusage)"
    local start=$(now) path seconds probe_deadline=$(($(now)+240))
    for path in "${BUILD_SOURCESDIRECTORY:+$BUILD_SOURCESDIRECTORY/.git}" \
        "$BUILD_SOURCESDIRECTORY" "$out" "$BUILD_STAGINGDIRECTORY" "$AGENT_TEMPDIRECTORY" \
        "${AGENT_WORKFOLDER:+$AGENT_WORKFOLDER/_tasks}" "$AGENT_TOOLSDIRECTORY" \
        "$HOME/.cargo" "$HOME/.rustup" "$HOME/.npm" "$HOME/.nuget" "$HOME/Library/Caches" \
        /System/Volumes/VM /private/var/folders; do
        seconds=20
        case "$path" in
            "$BUILD_SOURCESDIRECTORY"|"$BUILD_SOURCESDIRECTORY/.git"|"$out"|\
            "$AGENT_WORKFOLDER/_tasks"|"$AGENT_TOOLSDIRECTORY") seconds=60 ;;
        esac
        usage "$path" "$seconds"
    done
    emit du-seconds "$(($(now)-start))"
}

sampler_start() {
    local pid owner tries=0 lockdir=$pidfile.lock
    [ -d "${samples%/*}" ] || return
    while ! mkdir "$lockdir"; do
        owner=
        read -r owner < "$lockdir/owner"
        # Allow a concurrent mkdir winner to write its owner file.
        if [ -z "$owner" ]; then sleep 0.1; read -r owner < "$lockdir/owner"; fi
        if ! [[ $owner =~ ^[0-9]+$ ]] || ! kill -0 "$owner"; then
            rm -rf "$lockdir"
        fi
        tries=$((tries+1))
        if [ "$tries" -ge 20 ]; then emit sampler "startup lock busy"; return; fi
        sleep 0.1
    done
    printf '%s\n' "$$" > "$lockdir/owner"
    trap 'rm -rf "$pidfile.lock"' EXIT
    read -r pid < "$pidfile"
    if [[ $pid =~ ^[0-9]+$ ]] && kill -0 "$pid"; then emit sampler "already-running pid=$pid"; return; fi
    nohup /bin/bash "$0" sample-loop </dev/null >/dev/null 2>&1 &
    pid=$!
    disown "$pid"
    printf '%s\n' "$pid" > "$pidfile"
    emit sampler "started pid=$pid"
}

sample_loop() {
    local values swap_used
    while :; do
        values=$(bounded 10 df -Pk / | awk 'NR==2 && $(NF-4)~/^[0-9]+$/ && $(NF-4)>0 {printf "%s\t%s",$(NF-4),$(NF-2)}')
        swap_used=$(swap)
        [ -n "$values" ] && printf '%s\t%s\t%s\n' "$(now)" "$values" "${swap_used:-NA}" >> "$samples"
        sleep 10
    done
}

sampler_report() {
    local previous=0 count=0 scope
    read -r previous < "$marker"
    [[ $previous =~ ^[0-9]+$ ]] || previous=0
    count=$(wc -l < "$samples")
    [[ $count =~ ^[[:space:]]*[0-9]+[[:space:]]*$ ]] || count=0
    for scope in overall interval; do
        local skip=0 row n min t maxused usedtime maxswap
        [ "$scope" = interval ] && skip=$previous
        row=$(awk -v skip="$skip" -v end="$count" 'NR>skip && NR<=end && $2>0 && $3~/^[0-9]+$/ {
            used=100*(1-$3/$2)
            if(!n || $3<min) {min=$3; t=$1}
            if(!n || used>peak) {peak=used; pt=$1}
            if($4!="NA" && (!s || $4+0>sw)) {sw=$4+0; s=1}
            n++
        } END {if(n) printf "%d %s %s %.2f %s %s",n,min,t,peak,pt,(s ? sw : "NA")}' "$samples")
        if [ -n "$row" ]; then
            read -r n min t maxused usedtime maxswap <<< "$row"
            emit "$scope" "samples=$n min_available_KiB=$min min_available_utc=$(utc "$t") max_used_pct=$maxused max_used_utc=$(utc "$usedtime") max_swap_M=$maxswap"
        else emit "$scope" "samples=0"; fi
    done
    printf '%s\n' "$count" > "$marker"
    [ -f "$samples" ] && printf '##vso[task.uploadfile]%s\n' "$samples"
}

output_inventory() {
    local path
    for path in "$out" "$out/Release"; do
        [ -d "$path" ] || continue
        emit "top30:$path" "allocated_KiB (overlapping totals; 45-second limit)"
        bounded 45 du -k -d 2 "$path" | sort -nr | head -30 |
            while IFS= read -r line; do emit top30 "$line"; done
    done
    [ -n "$out" ] && [ -d "$out/Release" ] || return
    # Exclusive categories: Rust owns its subtree, including its objects/archives.
    bounded 180 find "$out/Release" -type f -exec stat -f '%z %b %Lp %N' {} + | awk -v l="$label" '
        BEGIN {split("objects archives dylibs executables dependencies generated rust other",k," ")}
        $1~/^[0-9]+$/ && $2~/^[0-9]+$/ {
            bytes=$1; blocks=$2; mode=$3; sub(/^[^ ]+ [^ ]+ [^ ]+ /,""); path=$0
            name=path; sub(/^.*\//,"",name); kind="other"
            if(path~/\/target\// || path~/\/obj\/gen\/(.*\/)?(release|debug|\.fingerprint|deps)\//) kind="rust"
            else if(path~/\.o$/) kind="objects"
            else if(path~/\.a$/) kind="archives"
            else if(path~/\.dylib$/) kind="dylibs"
            else if(path~/\.d$/ || path~/\/\.deps\//) kind="dependencies"
            else if(path~/\/obj\/gen\/.*\.(cc|cpp|h|S|dat)$/) kind="generated"
            else if(name!~/\./ && (int(mode/100)%2 || int(mode/10)%2 || mode%2)) kind="executables"
            n[kind]++; b[kind]+=bytes; a[kind]+=blocks/2
        }
        END {for(i=1;i<=8;i++) printf "DISK|%s|kind:%s|count=%d apparent_bytes=%.0f allocated_KiB=%.0f\n",l,k[i],n[k[i]],b[k[i]],a[k[i]]}'
}

dwarf_bytes() {
    local result status
    [ "$#" -gt 0 ] || { printf 'unavailable'; return; }
    result=$(printf '%s\0' "$@" | bounded 45 xargs -0 -n 30 sh -c '
        size -m "$@" || printf "\nsize_failure\n"
    ' sh); status=$?
    printf '%s\n' "$result" | awk -v status="$status" '
        /^size_failure$/ {failures++}
        /Segment / {
            named=($2=="__DWARF:")
            if(named) {sum+=$NF; found=1}
        }
        /Section \(__DWARF,/ {if(!named) sum+=$NF; found=1}
        END {
            if(found) printf "%.0f",sum; else printf "unavailable"
            if(failures || status) printf " failed_batches=%d status=%d",failures,status
        }'
}

debug_inventory() {
    [ -n "$out" ] && [ -d "$out" ] || return
    emit debug-flags "token counts in generated makefiles"
    bounded 60 find "$out" -type f \( -name '*.mk' -o -path "$out/Makefile" \) -exec cat {} + |
        awk '{for(i=1;i<=NF;i++) if($i~/^-g([0-3]|dwarf[^[:space:]]*|line-tables-only)?$/) n[$i]++}
            END {for(k in n) print k,n[k]}' |
        while IFS= read -r line; do emit debug-flag "$line"; done
    local objects=() path start limit count i batch=() total=0 value details measured=0 unavailable=0
    while IFS= read -r path; do objects[${#objects[@]}]=$path; done < <(
        bounded 180 find "$out/Release" -type f -exec stat -f '%z %b %Lp %N' {} + |
            awk '/\.o$/ {size=$1; sub(/^[^ ]+ [^ ]+ [^ ]+ /,""); print size "\t" $0}' |
            sort -nr | cut -f2-
    )
    count=${#objects[@]}
    start=$(now)
    limit=$count
    local probe_deadline=$(($(now)+240))
    if [ -n "$inventory_deadline" ] && [ "$probe_deadline" -gt "$inventory_deadline" ]; then
        probe_deadline=$inventory_deadline
    fi
    for ((i=0; i<limit; i+=30)); do
        [ "$(now)" -lt "$probe_deadline" ] || { emit object-DWARF "time limit reached; partial total"; break; }
        batch=("${objects[@]:i:30}")
        read -r value details <<< "$(dwarf_bytes "${batch[@]}")"
        [[ $value =~ ^[0-9]+$ ]] && total=$((total+value))
        if [[ $value =~ ^[0-9]+$ ]] && [ -z "$details" ]; then measured=$((measured+${#batch[@]}))
        elif [ -n "$details" ]; then
            unavailable=$((unavailable+${#batch[@]})); emit object-DWARF "$details"
        else unavailable=$((unavailable+${#batch[@]})); fi
    done
    emit object-DWARF "known_bytes=$total measured_objects=$measured unavailable_objects=$unavailable selected=$limit total_objects=$count elapsed_seconds=$(($(now)-start))"
    probe_deadline=$inventory_deadline
    while IFS= read -r -d '' path; do
        start=$(now)
        emit "DWARF:$path" "$(dwarf_bytes "$path") elapsed_seconds=$(($(now)-start))"
    done < <(bounded 30 find "$out/Release" -type f \( -name 'libnode*.dylib' -o -name embedtest -o -name shared_embedtest -o -name node \) -print0)
}

image_inventory() {
    local path sdk root seen roots=()
    bounded 10 sw_vers | while IFS= read -r line; do emit sw_vers "$line"; done
    emit hw.ncpu "$(bounded 10 sysctl -n hw.ncpu)"
    emit hw.memsize "$(bounded 10 sysctl -n hw.memsize)"
    emit ImageOS "${ImageOS:-unset}"; emit ImageVersion "${ImageVersion:-unset}"
    emit xcode-select "$(bounded 10 xcode-select -p)"
    bounded 30 diskutil apfs list | while IFS= read -r line; do emit apfs "$line"; done
    for path in /Applications/Xcode*.app; do
        if [ -L "$path" ]; then emit xcode-symlink "$path -> $(bounded 10 readlink "$path")"
        else usage "$path" 120; fi
    done
    for sdk in "$ANDROID_HOME" "$ANDROID_SDK_ROOT" "$HOME/Library/Android/sdk"; do
        [ -n "$sdk" ] && [ -d "$sdk" ] || continue
        root=$(cd "$sdk" && pwd -P) || continue
        seen=0
        for path in "${roots[@]}"; do [ "$path" = "$root" ] && seen=1; done
        [ "$seen" -eq 0 ] || continue
        roots[${#roots[@]}]=$root
        usage "$root" 120
        for path in "$root"/ndk/* "$root/ndk-bundle"; do usage "$path" 120; done
    done
    for path in "$HOME/Library/Developer" /Library/Developer/CoreSimulator \
        "$HOME/Library/Developer/CoreSimulator" /opt/homebrew; do usage "$path" 120; done
    for path in /usr/local \
        "$HOME/.dotnet" /usr/local/share/dotnet "$AGENT_TOOLSDIRECTORY" \
        "${AGENT_TOOLSDIRECTORY:-/nonexistent}"/* "$HOME/.cargo" "$HOME/.rustup"; do usage "$path"; done
}

inventory() {
    local section start inventory_deadline=$(($(now)+1200))
    local probe_deadline=$inventory_deadline
    for section in output_inventory debug_inventory image_inventory; do
        if [ "$(now)" -ge "$inventory_deadline" ]; then emit "$section" "status=skipped_budget"; continue; fi
        start=$(now); emit section-start "$section"
        "$section"
        emit "seconds:$section" "$(($(now)-start))"
    done
}

case "$1" in
    boundary) boundary ;;
    sampler-start) sampler_start ;;
    sample-loop) sample_loop ;;
    sampler-report) sampler_report ;;
    inventory) inventory ;;
    *) emit error "unknown subcommand" ;;
esac
exit 0
