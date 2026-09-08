"""Bounded public-archive resource and sustained-load acceptance on loopback."""
import argparse
import asyncio
import json
import math
import os
from pathlib import Path
import platform
import signal
import statistics
import subprocess
import tempfile
import time

import cluster as base

MAX_MESSAGES = 100_000
RSS_GROWTH_KIB = 16 * 1024
RSS_SLOPE_KIB_PER_MINUTE = 256


class Peer(base.Peer):
    """Keep payloads only until reconciliation; a fixed bitmap detects all duplicates."""
    def __init__(self, uid):
        super().__init__(uid)
        self.seen = bytearray(MAX_MESSAGES)
        self.received = self.blocked = 0

    def record_message(self, value):
        counter = value['payload']['counter']
        assert 0 < counter < MAX_MESSAGES and not self.seen[counter], 'Duplicate/out-of-budget message'
        assert len(self.messages) < 128, 'Unreconciled receive budget exceeded'
        self.seen[counter] = 1
        self.received += 1
        self.messages[counter] = value

    def extra_event(self, kind):
        if kind == 'blocked':
            self.blocked += 1
        else:
            super().extra_event(kind)


def measure(pid):
    """Read only one owned process. RSS is KiB; ps CPU is an OS sample, not SDK-only CPU."""
    def run(*argv):
        result = subprocess.run(argv, text=True, capture_output=True, timeout=5)
        assert result.returncode == 0, 'Owned-process resource observation failed'
        return result.stdout
    rss, cpu = run('ps', '-p', str(pid), '-o', 'rss=,pcpu=').split()
    if platform.system() == 'Darwin':
        threads = len(run('ps', '-M', '-p', str(pid)).strip().splitlines()) - 1
    else:
        threads = int(run('ps', '-p', str(pid), '-o', 'nlwp='))
    fields = run('lsof', '-nP', '-a', '-p', str(pid), '-FftPn').splitlines()
    descriptors = sum(line.startswith('f') and line[1:].isdigit() for line in fields)
    tcp, protocol = 0, None
    for line in fields:
        if line.startswith('f'):
            protocol = None
        elif line.startswith('P'):
            protocol = line[1:]
        elif line.startswith('n') and protocol == 'TCP' and '->' in line:
            tcp += 1
    assert int(rss) > 0 and threads > 0 and descriptors >= 3
    return dict(rss_kib=int(rss), ps_cpu_percent=float(cpu), threads=threads, descriptors=descriptors,
                tcp_connections=tcp)


def summarize(samples):
    """Compare warmed windows and fit RSS against elapsed time; never extrapolate a short run."""
    result = {}
    for role in samples[0]['processes']:
        rows = [(s['elapsed_seconds'], s['processes'][role]) for s in samples if role in s['processes']]
        values = [v for _, v in rows]
        times = [t / 60 for t, _ in rows]
        rss = [v['rss_kib'] for v in values]
        mean_t, mean_rss = statistics.mean(times), statistics.mean(rss)
        divisor = sum((t - mean_t) ** 2 for t in times)
        slope = sum((t - mean_t) * (r - mean_rss) for t, r in zip(times, rss)) / divisor if divisor else 0
        result[role] = {
            'samples': len(rows), 'rss_peak_kib': max(rss),
            'rss_window_growth_kib': statistics.median(rss[-5:]) - statistics.median(rss[:5]),
            'rss_slope_kib_per_minute': round(slope, 3),
            **{key + '_peak': max(v[key] for v in values)
               for key in ['threads', 'descriptors', 'tcp_connections', 'ps_cpu_percent']},
        }
    return result


class Ledger:
    """Bounded workload identities and exact ACK/RECV reconciliation, including ambiguous SENDs."""
    def __init__(self):
        self.attempts = self.acked = self.queue_full = self.ambiguous = 0
        self.latencies = []

    def new(self):
        self.attempts += 1
        assert self.attempts < MAX_MESSAGES, 'Attempt budget exceeded'
        return dict(clientMsgNo=f'cpp-soak-{self.attempts}', payload={
            'counter': self.attempts, 'type': 1, 'content': '持续收发 🌍 ' + 'x' * 256})

    async def reconcile(self, sender, receiver, message, outcome, latency):
        assert outcome['ok'], f"Unexpected SEND failure: {outcome.get('code')}"
        value = await receiver.message(message['payload']['counter'])
        ack = outcome['ack']
        assert value['payload'] == message['payload'] and value['fromUid'] == sender.uid
        assert ack['reasonCode'] == 1
        assert (ack['messageId'], ack['messageSeq']) == (value['messageId'], value['messageSeq'])
        receiver.messages.pop(message['payload']['counter'])
        self.acked += 1
        self.latencies.append(latency)

    async def exchange(self, sender, receiver):
        message, started = self.new(), time.monotonic()
        result = await sender.call('send', uid=receiver.uid, **message)
        await self.reconcile(sender, receiver, message, result, (time.monotonic() - started) * 1000)

    async def burst(self, sender, receiver, count, queue_full=0):
        messages = [self.new() for _ in range(count)]
        started = time.monotonic()
        result = await sender.call('burst', uid=receiver.uid, messages=messages)
        latency = (time.monotonic() - started) * 1000
        assert result['ok'] and len(result['results']) == count
        rejected = 0
        for message, outcome in zip(messages, result['results']):
            if outcome['ok']:
                await self.reconcile(sender, receiver, message, outcome, latency)
            else:
                assert outcome['code'] == -8, outcome
                assert not receiver.seen[message['payload']['counter']]
                rejected += 1
        assert rejected == queue_full, f'QueueFull count {rejected}, expected {queue_full}'
        self.queue_full += rejected


async def scenario(args, root, client, report):
    root.mkdir()
    cluster, peers = base.Cluster(root, args.server), []
    ledger, samples = Ledger(), []
    faults, background = [], []
    started = None
    sample_task = fault_task = None
    traffic_ready = asyncio.Event()
    traffic_ready.set()
    report.update(stage='startup', samples=samples, faults=faults)

    def stage(name, **values):
        report['stage'] = name
        print(json.dumps(dict(stage=name, **values)), flush=True)

    async def snapshot(elapsed):
        processes = {p.uid: p.process for p in peers}
        processes.update({f'server-{i+1}': p for i, p in enumerate(cluster.processes) if p.returncode is None})
        values = {}
        for role, process in processes.items():
            # A crash can race sampling; only the explicitly killed server may disappear.
            try:
                values[role] = await asyncio.to_thread(measure, process.pid)
            except AssertionError:
                if role == 'server-1' and process.returncode is not None:
                    continue
                raise
        return dict(elapsed_seconds=round(elapsed, 3), processes=values)

    async def sample_loop():
        while True:
            samples.append(await snapshot(time.monotonic() - started))
            await asyncio.sleep(args.sample_seconds)

    async def boundary():
        stage('slow_callback_concurrent_admission')
        blocked = alice.blocked
        assert (await alice.call('arm'))['ok']
        await ledger.exchange(carol, alice)
        await base.eventually(lambda: alice.blocked > blocked, 2)
        await asyncio.sleep(0.25)
        await ledger.burst(alice, carol, 64, queue_full=48)
        await ledger.exchange(alice, carol)

    async def fault(kind):
        began, connects = time.monotonic(), len(alice.connects)
        if kind == 'withheld_ack_timeout':
            proxy = cluster.proxies[0]
            message = ledger.new()
            proxy.withhold = True
            pending = asyncio.create_task(alice.call('send', uid=carol.uid, **message))
            background.append(pending)
            try:
                value = await carol.message(message['payload']['counter'])
                assert value['payload'] == message['payload'] and value['fromUid'] == alice.uid
                assert not pending.done(), 'SENDACK was not withheld'
                outcome = await pending
                request_seconds = time.monotonic() - began
                assert outcome.get('code') == -3 and not outcome['ok']
                assert 2.7 <= request_seconds < 6, 'Request deadline was not respected'
                carol.messages.pop(message['payload']['counter'])
                ledger.ambiguous += 1
                proxy.blocked = True
                proxy.abort()
                await asyncio.sleep(2)
            finally:
                proxy.blocked = proxy.withhold = False
        else:
            cluster.processes[0].kill()
            await asyncio.wait_for(cluster.processes[0].wait(), 5)
            await base.eventually(lambda: not alice.connected, 10)
            await cluster.stable([1, 2])
            traffic_ready.set()
            before = ledger.acked
            await asyncio.sleep(3)
            assert ledger.acked >= before + 2, 'Surviving-node traffic did not progress'
            await cluster.spawn(0)
            await cluster.ready(0)
            await cluster.stable([0, 1, 2])
        await base.eventually(lambda: alice.connected and len(alice.connects) > connects, 30)
        assert alice.connects[-1]['nodeId'] == 1
        await ledger.exchange(alice, carol)
        await ledger.exchange(carol, alice)
        faults.append(dict(kind=kind, recovery_seconds=round(time.monotonic() - began, 3)))
        stage('sustained_traffic', recovered=kind)

    try:
        await cluster.start()
        for i, uid in enumerate(['cpp-soak-alice', 'cpp-soak-bob', 'cpp-soak-carol', 'cpp-soak-churn']):
            node = min(i, 2)
            token = os.urandom(24).hex()
            await base.request(cluster.api(0), '/user/token',
                dict(uid=uid, token=token, device_flag=2, device_level=1))
            peer = Peer(uid)
            peers.append(peer)
            env = {k: v for k, v in os.environ.items() if not k.startswith(('WKIM_', 'NODE_TLS_'))}
            env.update(WKIM_URL=cluster.url(node), WKIM_UID=uid, WKIM_TOKEN=token,
                WKIM_CA_FILE=str(cluster.ca), NODE_EXTRA_CA_CERTS=str(cluster.ca), WKIM_JS_ENTRY=str(args.js_entry))
            await peer.start(['node', base.HERE / 'cluster_peer.cjs'] if i == 1 else [client], env, node + 1)
        alice, bob, carol, churn = peers
        cpp = [alice, carol, churn]
        initial = await base.request(cluster.api(1), '/user/onlinestatus', [p.uid for p in peers])
        assert len(initial) == 4 and all(v['online'] == 1 for v in initial)
        report['initial_online_routes'] = 4
        await boundary()
        stage('warmup')
        cycles = 0
        for _ in range(4):
            cycles += (await churn.call('recycle', count=5))['cycles']
        warmup_started = time.monotonic()
        while time.monotonic() - warmup_started < args.warmup_seconds:
            tick = time.monotonic()
            await ledger.burst(alice, carol, 8)
            await ledger.exchange(bob, alice)
            await ledger.exchange(carol, bob)
            await asyncio.sleep(max(0, 1 - (time.monotonic() - tick)))
        baseline = await snapshot(0)
        report['baseline'] = baseline
        started = time.monotonic()
        sample_task = asyncio.create_task(sample_loop())
        stage('sustained_traffic', duration_seconds=args.duration_seconds)
        next_progress, cycle_index, fault_index = 0, 0, 0
        soak_start_acked = ledger.acked
        while time.monotonic() - started < args.duration_seconds:
            tick = time.monotonic()
            elapsed = tick - started
            if sample_task.done():
                sample_task.result()
            for peer in peers:
                if peer.reader.done():
                    peer.reader.result()
                    raise AssertionError('Peer exited during sustained traffic')
            if fault_task and fault_task.done():
                fault_task.result()
                fault_task = None
            if not fault_task and fault_index < 2 and elapsed >= args.duration_seconds * (fault_index + 1) / 3:
                kind = ['withheld_ack_timeout', 'ingress_crash_restart'][fault_index]
                stage(kind)
                if kind == 'ingress_crash_restart':
                    # Fence the next traffic iteration before scheduling the crash.
                    traffic_ready.clear()
                fault_task = asyncio.create_task(fault(kind))
                background.append(fault_task)
                fault_index += 1
            if not traffic_ready.is_set():
                await base.eventually(lambda: traffic_ready.is_set() or fault_task.done(), 60)
                if fault_task.done():
                    fault_task.result()
            if fault_task:
                await ledger.exchange(bob, carol)
                await ledger.exchange(carol, bob)
            else:
                await ledger.burst(alice, carol, 8)
                await ledger.exchange(bob, alice)
                await ledger.exchange(carol, bob)
            if cycle_index < 12 and elapsed >= args.duration_seconds * cycle_index / 12:
                cycles += (await churn.call('recycle', count=5))['cycles']
                cycle_index += 1
            if elapsed >= next_progress:
                stage('sustained_traffic', elapsed_seconds=round(elapsed), acked=ledger.acked,
                      samples=len(samples), lifecycle_cycles=cycles)
                next_progress += 60
            await asyncio.sleep(max(0, 1 - (time.monotonic() - tick)))
        if fault_task:
            await fault_task
        report['sustained_seconds'] = round(time.monotonic() - started, 3)
        report['sustained_acked'] = ledger.acked - soak_start_acked
        assert report['sustained_acked'] >= args.duration_seconds * 5, 'Workload made insufficient progress'
        if sample_task.done():
            sample_task.result()
        sample_task.cancel()
        await asyncio.gather(sample_task, return_exceptions=True)
        samples.append(await snapshot(time.monotonic() - started))
        for _ in range(4):
            cycles += (await churn.call('recycle', count=5))['cycles']
        assert cycles == 100 and len(faults) == 2
        await boundary()
        await asyncio.sleep(1)
        final = await snapshot(time.monotonic() - started)
        report['final_connected'] = final
        summary = summarize(samples)
        report['resource_summary'] = summary
        for peer in cpp:
            name = peer.uid
            before, after = baseline['processes'][name], final['processes'][name]
            assert after['threads'] <= before['threads'] + 1, 'Idle worker accumulation'
            assert after['descriptors'] <= before['descriptors'] + 2, 'Descriptor accumulation'
            assert after['tcp_connections'] == before['tcp_connections'] == 1, 'Unexpected connection count'
            assert summary[name]['rss_window_growth_kib'] <= RSS_GROWTH_KIB, 'Warmed RSS growth exceeded bound'
            if args.duration_seconds >= 600:
                assert summary[name]['rss_slope_kib_per_minute'] <= RSS_SLOPE_KIB_PER_MINUTE, 'RSS trend exceeded bound'
        stage('destroyed_resource_cleanup')
        for peer in cpp:
            assert (await peer.call('park'))['destroyed']
        await asyncio.sleep(0.5)
        parked = {p.uid: await asyncio.to_thread(measure, p.process.pid) for p in cpp}
        report['parked_cpp_processes'] = parked
        assert all(v['threads'] == 1 and v['tcp_connections'] == 0 for v in parked.values()), 'SDK resources survived destroy'
        for peer in peers:
            assert (await peer.call('stop'))['destroyed']
            assert await asyncio.wait_for(peer.process.wait(), 5) == 0
        async with asyncio.timeout(20):
            while await base.request(cluster.api(1), '/user/onlinestatus', [p.uid for p in peers]):
                await asyncio.sleep(0.1)
        assert all(not p.pending and not p.messages for p in peers), 'Unsettled control or receive state'
        assert sum(p.received for p in peers) == ledger.acked + ledger.ambiguous, 'Unreconciled delivery'
        assert ledger.attempts == ledger.acked + ledger.queue_full + ledger.ambiguous
        latencies = sorted(ledger.latencies)
        report.update(stage='passed', lifecycle_cycles=cycles, attempts=ledger.attempts,
            acked=ledger.acked, queue_full=ledger.queue_full, ambiguous_delivered=ledger.ambiguous,
            received=sum(p.received for p in peers), presence_cleared=True,
            latency_ms={str(p): round(latencies[min(len(latencies)-1, math.ceil(len(latencies)*p/100)-1)], 3)
                        for p in [50, 95, 99, 100]},
            latency_definition='Control round-trip to SENDACK; burst members share batch completion time',
            result_kind='one_hour_soak' if args.duration_seconds == 3600 else 'short_acceptance')
    finally:
        for task in [sample_task, *background]:
            if task:
                task.cancel()
        await asyncio.gather(*(t for t in [sample_task, *background] if t), return_exceptions=True)
        await asyncio.gather(*(p.close() for p in peers))
        await cluster.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ['archive', 'server', 'js-entry', 'report']:
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--duration-seconds', type=int, choices=[120, 3600], default=120)
    parser.add_argument('--configuration', choices=['Debug', 'Release'], default='Release')
    args = parser.parse_args()
    args.configurations = [args.configuration]
    args.warmup_seconds = 60 if args.duration_seconds == 3600 else 10
    args.sample_seconds = 10 if args.duration_seconds == 3600 else 5
    for name in ['archive', 'server', 'js_entry', 'report']:
        setattr(args, name, getattr(args, name).resolve())
    report = dict(stage='preparation', configuration=args.configuration,
        requested_seconds=args.duration_seconds, warmup_seconds=args.warmup_seconds,
        sample_seconds=args.sample_seconds, host=dict(system=platform.system(), machine=platform.machine()),
        thresholds=dict(rss_window_growth_kib=RSS_GROWTH_KIB, rss_slope_kib_per_minute=RSS_SLOPE_KIB_PER_MINUTE,
                        slope_gate_minimum_seconds=600),
        topology=dict(nodes=3, hash_slots=256, logical_slots=12, slot_replicas=3, token_auth=True, transport='WSS'),
        workload='8 concurrent C++ SENDs plus two JS/C++ directions per paced second; surviving peers during faults')
    try:
        with tempfile.TemporaryDirectory(prefix='wk-cpp-soak-') as directory:
            root = Path(directory)
            clients, provenance = base.prepare(args, root)
            provenance['harness_sha256']['soak.py'] = base.digest(Path(__file__))
            report.update(provenance)
            async def run():
                loop, task = asyncio.get_running_loop(), asyncio.current_task()
                for sig in [signal.SIGTERM, signal.SIGINT]:
                    loop.add_signal_handler(sig, task.cancel)
                async with asyncio.timeout(args.duration_seconds + args.warmup_seconds + 240):
                    await scenario(args, root / 'cluster', clients[args.configuration].with_name('wukong_soak_peer'), report)
            asyncio.run(run())
        report['cleanup'] = 'All owned peers, proxies and nodes stopped; temporary credentials and data removed'
    except BaseException as error:
        report['failed_stage'] = report['stage']
        report['stage'] = 'failed'
        report['failure_type'] = type(error).__name__
        raise
    finally:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2) + '\n')
    print('Released C++ resource/soak acceptance passed', flush=True)


if __name__ == '__main__':
    main()
