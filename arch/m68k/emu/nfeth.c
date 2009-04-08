/*
 * atari_nfeth.c - ARAnyM ethernet card driver for GNU/Linux
 *
 * Copyright (c) 2005 Milan Jurik, Petr Stehlik of ARAnyM dev team
 *
 * Based on ARAnyM driver for FreeMiNT written by Standa Opichal
 *
 * This software may be used and distributed according to the terms of
 * the GNU General Public License (GPL), incorporated herein by reference.
 */

#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/module.h>
#include <asm/natfeat.h>
#include <asm/virtconvert.h>

enum {
	GET_VERSION = 0,	/* no parameters, return NFAPI_VERSION in d0 */
	XIF_INTLEVEL,		/* no parameters, return Interrupt Level in d0 */
	XIF_IRQ,		/* acknowledge interrupt from host */
	XIF_START,		/* (ethX), called on 'ifup', start receiver thread */
	XIF_STOP,		/* (ethX), called on 'ifdown', stop the thread */
	XIF_READLENGTH,		/* (ethX), return size of network data block to read */
	XIF_READBLOCK,		/* (ethX, buffer, size), read block of network data */
	XIF_WRITEBLOCK,		/* (ethX, buffer, size), write block of network data */
	XIF_GET_MAC,		/* (ethX, buffer, size), return MAC HW addr in buffer */
	XIF_GET_IPHOST,		/* (ethX, buffer, size), return IP address of host */
	XIF_GET_IPATARI,	/* (ethX, buffer, size), return IP address of atari */
	XIF_GET_NETMASK		/* (ethX, buffer, size), return IP netmask */
};

#define DRV_NAME	"nfeth"
#define DRV_VERSION	"0.3"
#define DRV_RELDATE	"10/12/2005"

#define MAX_UNIT	8

/* These identify the driver base version and may not be removed. */
static char version[] __devinitdata =
KERN_INFO DRV_NAME ".c:v" DRV_VERSION " " DRV_RELDATE " S.Opichal, M.Jurik, P.Stehlik\n"
KERN_INFO "  http://aranym.atari.org/\n";

MODULE_AUTHOR("Milan Jurik");
MODULE_DESCRIPTION("Atari NFeth driver");
MODULE_LICENSE("GPL");
/*
MODULE_PARM(nfeth_debug, "i");
MODULE_PARM_DESC(nfeth_debug, "nfeth_debug level (1-2)");
*/


static long nfEtherID;
static int nfEtherIRQ;

struct nfeth_private {
	int ethX;
	struct net_device_stats	stats;
};

static struct net_device *nfeth_dev[MAX_UNIT];

int nfeth_open(struct net_device *dev);
int nfeth_stop(struct net_device *dev);
irqreturn_t nfeth_interrupt(int irq, void *dev_id);
int nfeth_xmit(struct sk_buff *skb, struct net_device *dev);

int nfeth_open(struct net_device *dev)
{
	struct nfeth_private *priv = netdev_priv(dev);
	int res;

	res = nf_call(nfEtherID + XIF_START, priv->ethX);

	/* Clean statistics */
	memset(&priv->stats, 0, sizeof(struct net_device_stats));

	pr_debug(DRV_NAME ": open %d\n", res);

	/* Ready for data */
	netif_start_queue(dev);

	return 0;
}

int nfeth_stop(struct net_device *dev)
{
	struct nfeth_private *priv = netdev_priv(dev);

	/* No more data */
	netif_stop_queue(dev);

	nf_call(nfEtherID + XIF_STOP, priv->ethX);

	return 0;
}

/*
 * Read a packet out of the adapter and pass it to the upper layers
 */
static inline void recv_packet(struct net_device *dev)
{
	struct nfeth_private *priv = netdev_priv(dev);
	int handled = 0;
	unsigned short pktlen;
	struct sk_buff *skb;

	/* read packet length (excluding 32 bit crc) */
	pktlen = nf_call(nfEtherID + XIF_READLENGTH, priv->ethX);

	pr_debug(DRV_NAME ": recv_packet: %i\n", pktlen);

	if (!pktlen) {
		pr_debug(DRV_NAME ": recv_packet: pktlen == 0\n");
		priv->stats.rx_errors++;
		return;
	}

	skb = dev_alloc_skb(pktlen + 2);
	if (!skb) {
		pr_debug(DRV_NAME
			 ": recv_packet: out of mem (buf_alloc failed)\n");
		priv->stats.rx_dropped++;
		return;
	}

	skb->dev = dev;
	skb_reserve(skb, 2);		/* 16 Byte align  */
	skb_put(skb, pktlen);		/* make room */
	nf_call(nfEtherID + XIF_READBLOCK, priv->ethX, virt_to_phys(skb->data),
		pktlen);

	skb->protocol = eth_type_trans(skb, dev);
	netif_rx(skb);
	dev->last_rx = jiffies;
	priv->stats.rx_packets++;
	priv->stats.rx_bytes += pktlen;

	/* and enqueue packet */
	handled = 1;
	return;
}

irqreturn_t nfeth_interrupt(int irq, void *dev_id)
{
	int i, m, mask;

	mask = nf_call(nfEtherID + XIF_IRQ, 0);
	for (i = 0, m = 1; i < MAX_UNIT; m <<= 1, i++) {
		if (mask & m && nfeth_dev[i]) {
			recv_packet(nfeth_dev[i]);
			nf_call(nfEtherID + XIF_IRQ, m);
		}
	}
	return IRQ_HANDLED;
}

int nfeth_xmit(struct sk_buff *skb, struct net_device *dev)
{
	int len;
	char *data, shortpkt[ETH_ZLEN];
	struct nfeth_private *priv = netdev_priv(dev);

	data = skb->data;
	len = skb->len;
	if (len < ETH_ZLEN) {
		memset(shortpkt, 0, ETH_ZLEN);
		memcpy(shortpkt, data, len);
		data = shortpkt;
		len = ETH_ZLEN;
	}

	dev->trans_start = jiffies;

	pr_debug(DRV_NAME ": send %d bytes\n", len);
	nf_call(nfEtherID + XIF_WRITEBLOCK, priv->ethX, virt_to_phys(data),
		len);

	priv->stats.tx_packets++;
	priv->stats.tx_bytes += len;

	dev_kfree_skb(skb);
	return 0;
}

static void nfeth_tx_timeout(struct net_device *dev)
{
	struct nfeth_private *priv = netdev_priv(dev);
	priv->stats.tx_errors++;
	netif_wake_queue(dev);
}

static struct net_device_stats *nfeth_get_stats(struct net_device *dev)
{
	struct nfeth_private *priv = netdev_priv(dev);
	return &priv->stats;
}

struct net_device * __init nfeth_probe(int unit)
{
	struct net_device *dev;
	struct nfeth_private *priv;
	char mac[ETH_ALEN], host_ip[32], local_ip[32];
	DECLARE_MAC_BUF(macfmt);
	int err;

	if (!nf_call(nfEtherID + XIF_GET_MAC, unit, mac, ETH_ALEN))
		return NULL;

	dev = alloc_etherdev(sizeof(struct nfeth_private));
	if (!dev)
		return NULL;

	dev->irq = nfEtherIRQ;
	dev->open = nfeth_open;
	dev->stop = nfeth_stop;
	dev->hard_start_xmit = nfeth_xmit;
	dev->tx_timeout = nfeth_tx_timeout;
	dev->get_stats = nfeth_get_stats;
	dev->flags |= NETIF_F_NO_CSUM;
	memcpy(dev->dev_addr, mac, ETH_ALEN);

	priv = netdev_priv(dev);
	priv->ethX = unit;

	err = register_netdev(dev);
	if (err) {
		free_netdev(dev);
		return NULL;
	}

	nf_call(nfEtherID + XIF_GET_IPHOST, unit,
		host_ip, sizeof(host_ip));
	nf_call(nfEtherID + XIF_GET_IPATARI, unit,
		local_ip, sizeof(local_ip));

	pr_info("%s: nfeth addr:%s (%s) HWaddr:%s\n", dev->name, host_ip,
		local_ip, print_mac(macfmt, mac));

	return dev;
}

int __init nfeth_init(void)
{
	long ver;
	int i;

	nfEtherID = nf_get_id("ETHERNET");
	if (!nfEtherID)
		return -ENODEV;

	ver = nf_call(nfEtherID + GET_VERSION);
	pr_info("nfeth API %lu\n", ver);

	nfEtherIRQ = nf_call(nfEtherID + XIF_INTLEVEL);
	if (request_irq(nfEtherIRQ, nfeth_interrupt, IRQF_SHARED,
			"eth emu", nfeth_interrupt)) {
		printk(KERN_ERR "nfeth: request for irq %d failed",
		       nfEtherIRQ);
		return -ENODEV;
	}

	for (i = 0; i < MAX_UNIT; i++)
		nfeth_dev[i] = nfeth_probe(i);

	return 0;
}

void __exit nfeth_cleanup(void)
{
	int i;

	for (i = 0; i < MAX_UNIT; i++) {
		if (nfeth_dev[i]) {
			unregister_netdev(nfeth_dev[0]);
			free_netdev(nfeth_dev[0]);
		}
	}
	free_irq(nfEtherIRQ, nfeth_interrupt);
}

module_init(nfeth_init);
module_exit(nfeth_cleanup);
