package org.servalarch.servalctrl;

public class FlowTCPStat extends FlowStat {

	public long retrans;
	public long lost;
	public long rtt;
	public long rttvar;
	public long snd_una;
	public long snd_nxt;

	public FlowTCPStat(long flowId, int protocol, long pktsSent, long bytesSent, 
			long pktsRecv, long bytesRecv, long retrans, long lost, long rtt, 
			long rttvar, long snd_una, long snd_nxt) {
		super(flowId, protocol, pktsSent, bytesSent, pktsRecv, bytesRecv);
		this.retrans = retrans;
		this.lost = lost;
		this.rtt = rtt;
		this.rttvar = rttvar;
		this.snd_una = snd_una;
		this.snd_nxt = snd_nxt;
	}

	@Override
	public String toString() {
		return String.format("{pktsSent: %d, bytesSent: %d, pktsRecv: %d, bytesRecv: %d, retrans: %d, lost: %d, rtt: %d, rttvar: %d, "
				+ "snd_una: %d, snd_nxt: %d}", pktsSent, bytesSent, pktsRecv, bytesRecv, retrans, lost, rtt, rttvar,
				snd_una, snd_nxt);
	}
}
