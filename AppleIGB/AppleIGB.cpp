#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/network/IOEthernetController.h>
#include <IOKit/network/IOEthernetInterface.h>
#include <IOKit/network/IOGatedOutputQueue.h>
#include <IOKit/network/IOMbufMemoryCursor.h>
#include <IOKit/network/IOPacketQueue.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/assert.h>

extern "C" {
#include <sys/kpi_mbuf.h>
#include <net/ethernet.h>
}

extern "C" {
#include "igb.h"
}

#include "AppleIGB.h"


#define USE_HW_UDPCSUM 0
#define CAN_RECOVER_STALL	0

#define NETIF_F_TSO
#define NETIF_F_TSO6

/* IPv6 flags are not defined in 10.6 headers. */
enum {
	CSUM_TCPIPv6             = 0x0020,
	CSUM_UDPIPv6             = 0x0040
};

#define	RELEASE(x)	{if(x)x->release();x=NULL;}

static inline ip* ip_hdr(mbuf_t skb)
{
	return (ip*)((u8*)mbuf_data(skb)+ETHER_HDR_LEN);
}

static inline struct tcphdr* tcp_hdr(mbuf_t skb)
{
	struct ip* iph = ip_hdr(skb);
	return (struct tcphdr*)((u8*)iph + (iph->ip_hl << 2));
}

static inline struct ip6_hdr* ip6_hdr(mbuf_t skb)
{
	return (struct ip6_hdr*)((u8*)mbuf_data(skb)+ETHER_HDR_LEN);
}

static inline struct tcphdr* tcp6_hdr(mbuf_t skb)
{
	struct ip6_hdr* ip6 = ip6_hdr(skb);
	while(ip6->ip6_ctlun.ip6_un1.ip6_un1_nxt != IPPROTO_TCP){
		ip6++;
	}
	return (struct tcphdr*)(ip6+1);
}

static void* kzalloc(size_t size)
{
	void* p = IOMalloc(size);
	if(p){
		bzero(p, size);
	} else {
		pr_err("kzalloc: failed size = %d\n", (int)size );
	}
	return p;
}

static void* kcalloc(size_t num, size_t size)
{
	void* p = IOMalloc(num * size);
	if(p){
		bzero(p, num * size);
	} else {
		pr_err("kcalloc: failed num = %d, size = %d\n", (int)num, (int)size );
	}
	return p;
}

static void kfree(void* p, size_t size)
{
	IOFree(p, size);
}


static void* vzalloc(size_t size)
{
	void* p = IOMallocPageable(size, PAGE_SIZE);
	if(p){
		bzero(p, size);
	} else {
		pr_err("vzalloc: failed size = %d\n", (int)size );
	}
	return p;
}

static void vfree(void* p, size_t size)
{
	IOFreePageable(p, size);
}

static void netif_carrier_off(IOEthernetController* netdev){
	((AppleIGB*)netdev)->setCarrier(false);
}

static void netif_carrier_on(IOEthernetController* netdev){
	((AppleIGB*)netdev)->setCarrier(true);
}


static void netif_tx_start_all_queues(IOEthernetController* netdev){
	((AppleIGB*)netdev)->startTxQueue();
}

static void netif_tx_wake_all_queues(IOEthernetController* netdev){
	((AppleIGB*)netdev)->startTxQueue();
}


static void netif_tx_stop_all_queues(IOEthernetController* netdev){
	((AppleIGB*)netdev)->stopTxQueue();
}

static igb_adapter* netdev_priv(IOEthernetController* netdev)
{
	return ((AppleIGB*)netdev)->adapter();
}

static int netif_running(IOEthernetController* netdev)
{
	return ((AppleIGB*)netdev)->running();
}

static int netif_queue_stopped(IOEthernetController* netdev)
{
	return ((AppleIGB*)netdev)->queueStopped();
}

static int netif_carrier_ok(IOEthernetController* netdev)
{
	return ((AppleIGB*)netdev)->carrier();
}

static void netif_wake_queue(IOEthernetController* netdev)
{
    DEBUGFUNC("netif_wake_queue().\n");
	netif_tx_wake_all_queues(netdev);
}

static void netif_stop_queue(IOEthernetController* netdev)
{
    DEBUGFUNC("netif_stop_queue().\n");
	netif_tx_stop_all_queues(netdev);
}


static mbuf_t netdev_alloc_skb_ip_align(IOEthernetController* netdev, u16 rx_buffer_len)
{
    mbuf_t skb = netdev->allocatePacket(rx_buffer_len);
    mbuf_pkthdr_setlen(skb, 0);
	return skb;
}

static __be16 vlan_get_protocol(struct sk_buff *skb)
{
	iphdr* p = (iphdr*)mbuf_pkthdr_header(skb);
	return p->ip_p;
}

#define	jiffies	_jiffies()
static u64 _jiffies()
{
#if defined(MAC_OS_X_VERSION_10_6)
	clock_sec_t seconds;
	clock_usec_t microsecs;
#else
	uint32_t seconds;
	uint32_t microsecs;
#endif
	clock_get_system_microtime(&seconds, &microsecs);
	return  seconds * 100 + microsecs / 10000; // 10 ms
}
#define	HZ	250

static int time_after(u64 a, u64 b)
{
	if(a > b)
		return 1;
	return 0;
}

#define schedule_work(a)	(*(a))->setTimeoutMS(1)

static int pci_enable_device_mem(IOPCIDevice *dev)
{
	if(dev->setMemoryEnable(true))
		return 0;
	return -EINVAL;
}


#define	skb_record_rx_queue(skb,n)

#define PCI_MSI_FLAGS           2       /* Various flags */
#define PCI_MSI_FLAGS_QMASK    0x0e    /* Maximum queue size available */
static int pci_enable_msi_block(IOPCIDevice *dev )
{
	unsigned int nvec = 1;
	int status = -EINVAL, maxvec;
	u16 msgctl;

	u8 pos;

	if (dev->findPCICapability(kIOPCIMSICapability, &pos) == 0)
		return -EINVAL;
	msgctl = dev->configRead16(pos+PCI_MSI_FLAGS);
	maxvec = 1 << ((msgctl & PCI_MSI_FLAGS_QMASK) >> 1);
	if (nvec > maxvec)
		return maxvec;
	
#if 0
	status = pci_msi_check_device(dev, nvec, kIOPCIMSICapability);
	if (status)
		return status;
	
	/* Check whether driver already requested MSI-X irqs */
	if (dev->msix_enabled) {
		return -EINVAL;
	}
#endif
#if 0
	// OS specific chain
	status = msi_capability_init(dev, nvec);
#endif
	return status;
}

static int igb_setup_all_tx_resources(struct igb_adapter *);
static int igb_setup_all_rx_resources(struct igb_adapter *);
static void igb_free_all_tx_resources(struct igb_adapter *);
static void igb_free_all_rx_resources(struct igb_adapter *);
static void igb_setup_mrqc(struct igb_adapter *);
//static int igb_probe(IOPCIDevice*, const struct pci_device_id *);
//static void igb_remove(IOPCIDevice *pdev);
static int igb_sw_init(struct igb_adapter *);
static void igb_configure(struct igb_adapter *);
static void igb_configure_tx(struct igb_adapter *);
static void igb_configure_rx(struct igb_adapter *);
static void igb_clean_all_tx_rings(struct igb_adapter *);
static void igb_clean_all_rx_rings(struct igb_adapter *);
static void igb_clean_tx_ring(igb_ring *);
static void igb_set_rx_mode(IOEthernetController*);
static void igb_update_phy_info(unsigned long);
static void igb_watchdog(unsigned long);
static void igb_watchdog_task(struct work_struct *);
static void igb_dma_err_task(struct igb_adapter *adapter,IOTimerEventSource * src);
static void igb_dma_err_timer(unsigned long data);
static netdev_tx_t igb_xmit_frame(struct sk_buff *skb, IOEthernetController*);
static struct net_device_stats *igb_get_stats(IOEthernetController*);
static int igb_change_mtu(IOEthernetController*, int);
/* void igb_full_sync_mac_table(struct igb_adapter *adapter); */
static int igb_set_mac(IOEthernetController*, void *);
static void igb_set_uta(struct igb_adapter *adapter);
static irqreturn_t igb_intr(int irq, void *);
static irqreturn_t igb_intr_msi(int irq, void *);
static irqreturn_t igb_msix_other(int irq, void *);
static void igb_rar_set_qsel(struct igb_adapter *, u8 *, u32, u8);
static void igb_rar_set_qsel(struct igb_adapter *, u8 *, u32, u8);
static irqreturn_t igb_msix_ring(int irq, void *);
#ifdef IGB_DCA
static void igb_update_dca(struct igb_q_vector *);
static void igb_setup_dca(struct igb_adapter *);
#endif /* IGB_DCA */
static int igb_poll(struct igb_q_vector *, int);
static bool igb_clean_tx_irq(struct igb_q_vector *);
static bool igb_clean_rx_irq(struct igb_q_vector *, int);
static int igb_ioctl(IOEthernetController*, struct ifreq *, int cmd);
static void igb_tx_timeout(IOEthernetController*);
static void igb_reset_task(struct work_struct *);
#ifdef HAVE_VLAN_RX_REGISTER
static void igb_vlan_mode(IOEthernetController*, struct vlan_group *);
#endif
#ifdef HAVE_INT_NDO_VLAN_RX_ADD_VID
#ifdef NETIF_F_HW_VLAN_CTAG_RX
static int igb_vlan_rx_add_vid(struct net_device *,
                               __always_unused __be16 proto, u16);
static int igb_vlan_rx_kill_vid(struct net_device *,
                                __always_unused __be16 proto, u16);
#else
static int igb_vlan_rx_add_vid(struct net_device *, u16);
static int igb_vlan_rx_kill_vid(struct net_device *, u16);
#endif
#else
static void igb_vlan_rx_add_vid(struct net_device *, u16);
static void igb_vlan_rx_kill_vid(struct net_device *, u16);
#endif
static void igb_restore_vlan(struct igb_adapter *);
static void igb_ping_all_vfs(struct igb_adapter *);
static void igb_msg_task(struct igb_adapter *);
static void igb_vmm_control(struct igb_adapter *);
static int igb_set_vf_mac(struct igb_adapter *, int, unsigned char *);
static void igb_restore_vf_multicasts(struct igb_adapter *adapter);
static void igb_process_mdd_event(struct igb_adapter *);
#ifdef IFLA_VF_MAX
static int igb_ndo_set_vf_mac(IOEthernetController* netdev, int vf, u8 *mac);
static int igb_ndo_set_vf_vlan(IOEthernetController *netdev,
								int vf, u16 vlan, u8 qos);
#ifdef HAVE_VF_SPOOFCHK_CONFIGURE
static int igb_ndo_set_vf_spoofchk(IOEthernetController *netdev, int vf,
								   bool setting);
#endif
#ifdef HAVE_NDO_SET_VF_MIN_MAX_TX_RATE
static int igb_ndo_set_vf_bw(IOEthernetController *netdev, int vf,
							 int min_tx_rate, int tx_rate);
#else
static int igb_ndo_set_vf_bw(IOEthernetController *netdev, int vf, int tx_rate);
#endif /* HAVE_NDO_SET_VF_MIN_MAX_TX_RATE */
static int igb_ndo_get_vf_config(IOEthernetController *netdev, int vf,
								 struct ifla_vf_info *ivi);
static void igb_check_vf_rate_limit(struct igb_adapter *);
#endif
static int igb_vf_configure(struct igb_adapter *adapter, int vf);
#ifdef CONFIG_PM
#ifdef HAVE_SYSTEM_SLEEP_PM_OPS
static int igb_suspend(struct device *dev);
static int igb_resume(struct device *dev);
#ifdef CONFIG_PM_RUNTIME
static int igb_runtime_suspend(struct device *dev);
static int igb_runtime_resume(struct device *dev);
static int igb_runtime_idle(struct device *dev);
#endif /* CONFIG_PM_RUNTIME */
static const struct dev_pm_ops igb_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(igb_suspend, igb_resume)
#ifdef CONFIG_PM_RUNTIME
	SET_RUNTIME_PM_OPS(igb_runtime_suspend, igb_runtime_resume,
					   igb_runtime_idle)
#endif /* CONFIG_PM_RUNTIME */
};
#else
static int igb_suspend(struct pci_dev *pdev, pm_message_t state);
static int igb_resume(struct pci_dev *pdev);
#endif /* HAVE_SYSTEM_SLEEP_PM_OPS */
#endif
#ifndef USE_REBOOT_NOTIFIER
static void igb_shutdown(IOPCIDevice*);
#else
static int igb_notify_reboot(struct notifier_block *, unsigned long, void *);
static struct notifier_block igb_notifier_reboot = {
	.notifier_call	= igb_notify_reboot,
	.next		= NULL,
	.priority	= 0
};
#endif
#ifdef IGB_DCA
static int igb_notify_dca(struct notifier_block *, unsigned long, void *);
static struct notifier_block dca_notifier = {
	.notifier_call	= igb_notify_dca,
	.next		= NULL,
	.priority	= 0
};
#endif

static void igb_init_fw(struct igb_adapter *adapter);
static void igb_init_dmac(struct igb_adapter *adapter, u32 pba);


/* u32 e1000_read_reg(struct e1000_hw *hw, u32 reg); */

static void igb_vfta_set(struct igb_adapter *adapter, u32 vid, bool add)
{
	struct e1000_hw *hw = &adapter->hw;
	struct e1000_host_mng_dhcp_cookie *mng_cookie = &hw->mng_cookie;
	u32 index = (vid >> E1000_VFTA_ENTRY_SHIFT) & E1000_VFTA_ENTRY_MASK;
	u32 mask = 1 << (vid & E1000_VFTA_ENTRY_BIT_SHIFT_MASK);
	u32 vfta;
	
	/*
	 * if this is the management vlan the only option is to add it in so
	 * that the management pass through will continue to work
	 */
	if ((mng_cookie->status & E1000_MNG_DHCP_COOKIE_STATUS_VLAN) &&
	    (vid == mng_cookie->vlan_id))
		add = TRUE;

	vfta = adapter->shadow_vfta[index];
	
	if (add)
		vfta |= mask;
	else
		vfta &= ~mask;
	
	e1000_write_vfta(hw, index, vfta);
	adapter->shadow_vfta[index] = vfta;
}

#define Q_IDX_82576(i) (((i & 0x1) << 3) + (i >> 1))
/**
 * igb_cache_ring_register - Descriptor ring to register mapping
 * @adapter: board private structure to initialize
 *
 * Once we know the feature-set enabled for the device, we'll cache
 * the register offset the descriptor ring is assigned to.
 **/
static void igb_cache_ring_register(struct igb_adapter *adapter)
{
	int i = 0, j = 0;
	u32 rbase_offset = adapter->vfs_allocated_count;
	
	if (adapter->hw.mac.type == e1000_82576) {
		/* The queues are allocated for virtualization such that VF 0
		 * is allocated queues 0 and 8, VF 1 queues 1 and 9, etc.
		 * In order to avoid collision we start at the first free queue
		 * and continue consuming queues in the same sequence
		 */
		if ((adapter->rss_queues > 1) && adapter->vmdq_pools) {
			for (; i < adapter->rss_queues; i++)
				adapter->rx_ring[i]->reg_idx = rbase_offset +
				Q_IDX_82576(i);
		}
	}
	for (; i < adapter->num_rx_queues; i++)
		adapter->rx_ring[i]->reg_idx = rbase_offset + i;
	for (; j < adapter->num_tx_queues; j++)
		adapter->tx_ring[j]->reg_idx = rbase_offset + j;
}

u32 e1000_read_reg(struct e1000_hw *hw, u32 reg)
{
	//struct igb_adapter *igb = container_of(hw, struct igb_adapter, hw);
	u8 __iomem *hw_addr = ACCESS_ONCE(hw->hw_addr);
	u32 value = 0;
	
    if (E1000_REMOVED(hw_addr)) {
        pr_err("hw_addr removed, can't read\n");
        return ~value;
    }

	value = readl(&hw_addr[reg]);
	
	/* reads should not return all F's */
	if (!(~value) && (!reg || !(~readl(hw_addr)))) {
		//AppleIGB *netdev = igb->netdev;

		hw->hw_addr = NULL;
		//netif_device_detach(netdev); @todo
		pr_err("PCIe link lost, device now detached\n");
	}
	
	return value;
}

static void igb_configure_lli(struct igb_adapter *adapter)
{
	struct e1000_hw *hw = &adapter->hw;
	u16 port;
	
	/* LLI should only be enabled for MSI-X or MSI interrupts */
	if (!adapter->msix_entries && !(adapter->flags & IGB_FLAG_HAS_MSI))
		return;
	
	if (adapter->lli_port) {
		/* use filter 0 for port */
		port = htons((u16)adapter->lli_port);
		E1000_WRITE_REG(hw, E1000_IMIR(0),
						(port | E1000_IMIR_PORT_IM_EN));
		E1000_WRITE_REG(hw, E1000_IMIREXT(0),
						(E1000_IMIREXT_SIZE_BP | E1000_IMIREXT_CTRL_BP));
	}
	
	if (adapter->flags & IGB_FLAG_LLI_PUSH) {
		/* use filter 1 for push flag */
		E1000_WRITE_REG(hw, E1000_IMIR(1),
						(E1000_IMIR_PORT_BP | E1000_IMIR_PORT_IM_EN));
		E1000_WRITE_REG(hw, E1000_IMIREXT(1),
						(E1000_IMIREXT_SIZE_BP | E1000_IMIREXT_CTRL_PSH));
	}
	
	if (adapter->lli_size) {
		/* use filter 2 for size */
		E1000_WRITE_REG(hw, E1000_IMIR(2),
						(E1000_IMIR_PORT_BP | E1000_IMIR_PORT_IM_EN));
		E1000_WRITE_REG(hw, E1000_IMIREXT(2),
						(adapter->lli_size | E1000_IMIREXT_CTRL_BP));
	}
	
}

/**
 *  igb_write_ivar - configure ivar for given MSI-X vector
 *  @hw: pointer to the HW structure
 *  @msix_vector: vector number we are allocating to a given ring
 *  @index: row index of IVAR register to write within IVAR table
 *  @offset: column offset of in IVAR, should be multiple of 8
 *
 *  This function is intended to handle the writing of the IVAR register
 *  for adapters 82576 and newer.  The IVAR table consists of 2 columns,
 *  each containing an cause allocation for an Rx and Tx ring, and a
 *  variable number of rows depending on the number of queues supported.
 **/
static void igb_write_ivar(struct e1000_hw *hw, int msix_vector,
						   int index, int offset)
{
	u32 ivar = E1000_READ_REG_ARRAY(hw, E1000_IVAR0, index);
	
	/* clear any bits that are currently set */
	ivar &= ~((u32)0xFF << offset);
	
	/* write vector and valid bit */
	ivar |= (msix_vector | E1000_IVAR_VALID) << offset;
	
	E1000_WRITE_REG_ARRAY(hw, E1000_IVAR0, index, ivar);
}

#define IGB_N0_QUEUE -1
static void igb_assign_vector(struct igb_q_vector *q_vector, int msix_vector)
{
	struct igb_adapter *adapter = q_vector->adapter;
	struct e1000_hw *hw = &adapter->hw;
	int rx_queue = IGB_N0_QUEUE;
	int tx_queue = IGB_N0_QUEUE;
	u32 msixbm = 0;
	
	if (q_vector->rx.ring)
		rx_queue = q_vector->rx.ring->reg_idx;
	if (q_vector->tx.ring)
		tx_queue = q_vector->tx.ring->reg_idx;
	
	switch (hw->mac.type) {
		case e1000_82575:
			/* The 82575 assigns vectors using a bitmask, which matches the
			 * bitmask for the EICR/EIMS/EIMC registers.  To assign one
			 * or more queues to a vector, we write the appropriate bits
			 * into the MSIXBM register for that vector.
			 */
			if (rx_queue > IGB_N0_QUEUE)
				msixbm = E1000_EICR_RX_QUEUE0 << rx_queue;
			if (tx_queue > IGB_N0_QUEUE)
				msixbm |= E1000_EICR_TX_QUEUE0 << tx_queue;
			if (!adapter->msix_entries && msix_vector == 0)
				msixbm |= E1000_EIMS_OTHER;
			E1000_WRITE_REG_ARRAY(hw, E1000_MSIXBM(0), msix_vector, msixbm);
			q_vector->eims_value = msixbm;
			break;
		case e1000_82576:
			/*
			 * 82576 uses a table that essentially consists of 2 columns
			 * with 8 rows.  The ordering is column-major so we use the
			 * lower 3 bits as the row index, and the 4th bit as the
			 * column offset.
			 */
			if (rx_queue > IGB_N0_QUEUE)
				igb_write_ivar(hw, msix_vector,
							   rx_queue & 0x7,
							   (rx_queue & 0x8) << 1);
			if (tx_queue > IGB_N0_QUEUE)
				igb_write_ivar(hw, msix_vector,
							   tx_queue & 0x7,
							   ((tx_queue & 0x8) << 1) + 8);
			q_vector->eims_value = 1 << msix_vector;
			break;
		case e1000_82580:
		case e1000_i350:
		case e1000_i354:
		case e1000_i210:
		case e1000_i211:
			/*
			 * On 82580 and newer adapters the scheme is similar to 82576
			 * however instead of ordering column-major we have things
			 * ordered row-major.  So we traverse the table by using
			 * bit 0 as the column offset, and the remaining bits as the
			 * row index.
			 */
			if (rx_queue > IGB_N0_QUEUE)
				igb_write_ivar(hw, msix_vector,
							   rx_queue >> 1,
							   (rx_queue & 0x1) << 4);
			if (tx_queue > IGB_N0_QUEUE)
				igb_write_ivar(hw, msix_vector,
							   tx_queue >> 1,
							   ((tx_queue & 0x1) << 4) + 8);
			q_vector->eims_value = 1 << msix_vector;
			break;
		default:
			BUG();
			break;
	}
	
	/* add q_vector eims value to global eims_enable_mask */
	adapter->eims_enable_mask |= q_vector->eims_value;
	
	/* configure q_vector to set itr on first interrupt */
	q_vector->set_itr = 1;
}

/**
 * igb_configure_msix - Configure MSI-X hardware
 *
 * igb_configure_msix sets up the hardware to properly
 * generate MSI-X interrupts.
 **/
static void igb_configure_msix(struct igb_adapter *adapter)
{
	u32 tmp;
	int i, vector = 0;
	struct e1000_hw *hw = &adapter->hw;
	
	adapter->eims_enable_mask = 0;
	
	/* set vector for other causes, i.e. link changes */
	switch (hw->mac.type) {
		case e1000_82575:
			tmp = E1000_READ_REG(hw, E1000_CTRL_EXT);
			/* enable MSI-X PBA support*/
			tmp |= E1000_CTRL_EXT_PBA_CLR;
			
			/* Auto-Mask interrupts upon ICR read. */
			tmp |= E1000_CTRL_EXT_EIAME;
			tmp |= E1000_CTRL_EXT_IRCA;
			
			E1000_WRITE_REG(hw, E1000_CTRL_EXT, tmp);
			
			/* enable msix_other interrupt */
			E1000_WRITE_REG_ARRAY(hw, E1000_MSIXBM(0), vector++,
								  E1000_EIMS_OTHER);
			adapter->eims_other = E1000_EIMS_OTHER;
			
			break;
			
		case e1000_82576:
		case e1000_82580:
		case e1000_i350:
		case e1000_i354:
		case e1000_i210:
		case e1000_i211:
			/* Turn on MSI-X capability first, or our settings
			 * won't stick.  And it will take days to debug.
			 */
			E1000_WRITE_REG(hw, E1000_GPIE, E1000_GPIE_MSIX_MODE |
							E1000_GPIE_PBA | E1000_GPIE_EIAME |
							E1000_GPIE_NSICR);
			
			/* enable msix_other interrupt */
			adapter->eims_other = 1 << vector;
			tmp = (vector++ | E1000_IVAR_VALID) << 8;
			
			E1000_WRITE_REG(hw, E1000_IVAR_MISC, tmp);
			break;
		default:
			/* do nothing, since nothing else supports MSI-X */
			break;
	} /* switch (hw->mac.type) */
	
	adapter->eims_enable_mask |= adapter->eims_other;
	
	for (i = 0; i < adapter->num_q_vectors; i++)
		igb_assign_vector(adapter->q_vector[i], vector++);
	
	E1000_WRITE_FLUSH(hw);
}

/**
 * igb_request_msix - Initialize MSI-X interrupts
 *
 * igb_request_msix allocates MSI-X vectors and requests interrupts from the
 * kernel.
 **/
static int igb_request_msix(struct igb_adapter *adapter)
{
#ifdef __APPLE__
	return -1;
#else
	IOEthernetController* netdev = adapter->netdev;
	int i, err = 0, vector = 0, free_vector = 0;
	
	err = request_irq(adapter->msix_entries[vector].vector,
	                  &igb_msix_other, 0, netdev->name, adapter);
	if (err)
		goto err_out;
	vector++;
	
	for (i = 0; i < adapter->num_q_vectors; i++) {
		struct igb_q_vector *q_vector = adapter->q_vector[i];
		vector++;

		q_vector->itr_register = adapter->io_addr + E1000_EITR(vector);
		
		if (q_vector->rx.ring && q_vector->tx.ring)
			sprintf(q_vector->name, "%s-TxRx-%u", netdev->name,
			        q_vector->rx.ring->queue_index);
		else if (q_vector->tx.ring)
			sprintf(q_vector->name, "%s-tx-%u", netdev->name,
			        q_vector->tx.ring->queue_index);
		else if (q_vector->rx.ring)
			sprintf(q_vector->name, "%s-rx-%u", netdev->name,
			        q_vector->rx.ring->queue_index);
		else
			sprintf(q_vector->name, "%s-unused", netdev->name);
		
		err = request_irq(adapter->msix_entries[vector].vector,
		                  igb_msix_ring, 0, q_vector->name,
		                  q_vector);
		if (err)
			goto err_free;
		vector++;
	}
	
	igb_configure_msix(adapter);
	return 0;

err_free:
	/* free already assigned IRQs */
	free_irq(adapter->msix_entries[free_vector++].vector, adapter);
	
	vector--;
	for (i = 0; i < vector; i++) {
		free_irq(adapter->msix_entries[free_vector++].vector,
				 adapter->q_vector[i]);
	}
err_out:
	return err;
#endif
}

/**
 * igb_free_q_vector - Free memory allocated for specific interrupt vector
 * @adapter: board private structure to initialize
 * @v_idx: Index of vector to be freed
 *
 * This function frees the memory allocated to the q_vector.
 **/
static void igb_free_q_vector(struct igb_adapter *adapter, int v_idx)
{
	struct igb_q_vector *q_vector = adapter->q_vector[v_idx];
	
	adapter->q_vector[v_idx] = NULL;
	
	/* igb_get_stats64() might access the rings on this vector,
	 * we must wait a grace period before freeing it.
	 */
	kfree(q_vector, q_vector->alloc_size);
	
#ifndef IGB_NO_LRO
	__skb_queue_purge(&q_vector->lrolist.active);
#endif
}

/**
 *  igb_reset_q_vector - Reset config for interrupt vector
 *  @adapter: board private structure to initialize
 *  @v_idx: Index of vector to be reset
 *
 *  If NAPI is enabled it will delete any references to the
 *  NAPI struct. This is preparation for igb_free_q_vector.
 **/
static void igb_reset_q_vector(struct igb_adapter *adapter, int v_idx)
{
	struct igb_q_vector *q_vector = adapter->q_vector[v_idx];
	
	/* if we're coming from igb_set_interrupt_capability, the vectors are
	 * not yet allocated
	 */
	if (!q_vector)
		return;
	
	if (q_vector->tx.ring)
		adapter->tx_ring[q_vector->tx.ring->queue_index] = NULL;
	
	if (q_vector->rx.ring)
		adapter->rx_ring[q_vector->rx.ring->queue_index] = NULL;
	
	//netif_napi_del(&q_vector->napi);
	
}

static void igb_reset_interrupt_capability(struct igb_adapter *adapter)
{
	int v_idx = adapter->num_q_vectors;

	if (adapter->msix_entries) {
		//pci_disable_msix(adapter->pdev);
		kfree(adapter->msix_entries,sizeof(struct msix_entry));
		adapter->msix_entries = NULL;
	} else if (adapter->flags & IGB_FLAG_HAS_MSI) {
		//pci_disable_msi(adapter->pdev);
	}
	while (v_idx--)
		igb_reset_q_vector(adapter, v_idx);
}

/**
 * igb_free_q_vectors - Free memory allocated for interrupt vectors
 * @adapter: board private structure to initialize
 *
 * This function frees the memory allocated to the q_vectors.  In addition if
 * NAPI is enabled it will delete any references to the NAPI struct prior
 * to freeing the q_vector.
 **/
static void igb_free_q_vectors(struct igb_adapter *adapter)
{
	int v_idx = adapter->num_q_vectors;
	
	adapter->num_tx_queues = 0;
	adapter->num_rx_queues = 0;
	adapter->num_q_vectors = 0;
	
	while (v_idx--) {
		igb_reset_q_vector(adapter, v_idx);
		igb_free_q_vector(adapter, v_idx);
	}
}

/**
 * igb_clear_interrupt_scheme - reset the device to a state of no interrupts
 *
 * This function resets the device so that it has 0 rx queues, tx queues, and
 * MSI-X interrupts allocated.
 */
static void igb_clear_interrupt_scheme(struct igb_adapter *adapter)
{
	igb_free_q_vectors(adapter);
	igb_reset_interrupt_capability(adapter);
}

/**
 * igb_process_mdd_event
 * @adapter - board private structure
 *
 * Identify a malicious VF, disable the VF TX/RX queues and log a message.
 */
static void igb_process_mdd_event(struct igb_adapter *adapter)
{
	struct e1000_hw *hw = &adapter->hw;
	u32 lvmmc, vfte, vfre, mdfb;
	u8 vf_queue;
	
	lvmmc = E1000_READ_REG(hw, E1000_LVMMC);
	vf_queue = lvmmc >> 29;
	
	/* VF index cannot be bigger or equal to VFs allocated */
	if (vf_queue >= adapter->vfs_allocated_count)
		return;
#ifndef __APPLE__
	netdev_info(adapter->netdev,
	            "VF %d misbehaved. VF queues are disabled. "
	            "VM misbehavior code is 0x%x\n", vf_queue, lvmmc);
#endif

	/* Disable VFTE and VFRE related bits */
	vfte = E1000_READ_REG(hw, E1000_VFTE);
	vfte &= ~(1 << vf_queue);
	E1000_WRITE_REG(hw, E1000_VFTE, vfte);
	
	vfre = E1000_READ_REG(hw, E1000_VFRE);
	vfre &= ~(1 << vf_queue);
	E1000_WRITE_REG(hw, E1000_VFRE, vfre);
	
	/* Disable MDFB related bit. Clear on write */
	mdfb = E1000_READ_REG(hw, E1000_MDFB);
	mdfb |= (1 << vf_queue);
	E1000_WRITE_REG(hw, E1000_MDFB, mdfb);
	
	/* Reset the specific VF */
	E1000_WRITE_REG(hw, E1000_VTCTRL(vf_queue), E1000_VTCTRL_RST);
}

/**
 * igb_disable_mdd
 * @adapter - board private structure
 *
 * Disable MDD behavior in the HW
 **/
static void igb_disable_mdd(struct igb_adapter *adapter)
{
	struct e1000_hw *hw = &adapter->hw;
	u32 reg;
	
	if ((hw->mac.type != e1000_i350) &&
	    (hw->mac.type != e1000_i354))
		return;
	
	reg = E1000_READ_REG(hw, E1000_DTXCTL);
	reg &= (~E1000_DTXCTL_MDP_EN);
	E1000_WRITE_REG(hw, E1000_DTXCTL, reg);
}

/**
 * igb_enable_mdd
 * @adapter - board private structure
 *
 * Enable the HW to detect malicious driver and sends an interrupt to
 * the driver.
 **/
static void igb_enable_mdd(struct igb_adapter *adapter)
{
	struct e1000_hw *hw = &adapter->hw;
	u32 reg;
	
	/* Only available on i350 device */
	if (hw->mac.type != e1000_i350)
		return;
	
	reg = E1000_READ_REG(hw, E1000_DTXCTL);
	reg |= E1000_DTXCTL_MDP_EN;
	E1000_WRITE_REG(hw, E1000_DTXCTL, reg);
}

/**
 * igb_reset_sriov_capability - disable SR-IOV if enabled
 *
 * Attempt to disable single root IO virtualization capabilites present in the
 * kernel.
 **/
static void igb_reset_sriov_capability(struct igb_adapter *adapter)
{
	//IOPCIDevice *pdev = adapter->pdev;
	struct e1000_hw *hw = &adapter->hw;
	
	/* reclaim resources allocated to VFs */
	if (adapter->vf_data) {
#ifndef	__APPLE__
		if (!pci_vfs_assigned(adapter)) {
			/*
			 * disable iov and allow time for transactions to
			 * clear
			 */
			pci_disable_sriov(pdev);
			msleep(500); // @todo
			pr_err("IOV Disabled\n");
		}
#endif

		/* Disable Malicious Driver Detection */
		igb_disable_mdd(adapter);
		
		/* free vf data storage */
		kfree(adapter->vf_data,sizeof(struct vf_data_storage));
		adapter->vf_data = NULL;
		
		/* switch rings back to PF ownership */
		E1000_WRITE_REG(hw, E1000_IOVCTL, E1000_IOVCTL_REUSE_VFQ);
		E1000_WRITE_FLUSH(hw);
		msleep(100);
		
	}
	
	adapter->vfs_allocated_count = 0;
}

/**
 * igb_set_sriov_capability - setup SR-IOV if supported
 *
 * Attempt to enable single root IO virtualization capabilites present in the
 * kernel.
 **/
static void igb_set_sriov_capability(struct igb_adapter *adapter)
{
	//IOPCIDevice *pdev = adapter->pdev;
	int old_vfs = 0;
#ifndef __APPLE__
	int i;

	old_vfs = pci_num_vf(pdev);
#endif
	if (old_vfs) {
		pr_err(	"%d pre-allocated VFs found - override max_vfs setting of %d\n",
			  old_vfs,adapter->vfs_allocated_count);
		adapter->vfs_allocated_count = old_vfs;
 	}
	/* no VFs requested, do nothing */
	if (!adapter->vfs_allocated_count)
		return;

	/* allocate vf data storage */
	adapter->vf_data = (vf_data_storage*)kcalloc(adapter->vfs_allocated_count,
	                           sizeof(struct vf_data_storage));
	
	if (adapter->vf_data) {
#ifndef __APPLE__
		if (!old_vfs) {
			if (pci_enable_sriov(pdev,
								 adapter->vfs_allocated_count))
				goto err_out;
			dev_warn(pci_dev_to_dev(pdev),
					 "SR-IOV has been enabled: configure port VLANs to keep your VFs secure\n");
		}
		for (i = 0; i < adapter->vfs_allocated_count; i++)
			igb_vf_configure(adapter, i);
		
		switch (adapter->hw.mac.type) {
            case e1000_82576:
            case e1000_i350:
                /* Enable VM to VM loopback by default */
                adapter->flags |= IGB_FLAG_LOOPBACK_ENABLE;
                break;
            default:
                /* Currently no other hardware supports loopback */
                break;
		}

		/* DMA Coalescing is not supported in IOV mode. */
		if (adapter->hw.mac.type >= e1000_i350)
			adapter->dmac = IGB_DMAC_DISABLE;
		if (adapter->hw.mac.type < e1000_i350)
			adapter->flags |= IGB_FLAG_DETECT_BAD_DMA;
		return;
#endif
		
		kfree(adapter->vf_data, sizeof(struct vf_data_storage));
		adapter->vf_data = NULL;
	}
	
err_out:
	kfree(adapter->vf_data, sizeof(struct vf_data_storage));
	adapter->vf_data = NULL;
	adapter->vfs_allocated_count = 0;
	pr_err("Failed to initialize SR-IOV virtualization\n");
}

/**
 * igb_set_interrupt_capability - set MSI or MSI-X if supported
 *
 * Attempt to configure interrupts using the best available
 * capabilities of the hardware and kernel.
 **/
static void igb_set_interrupt_capability(struct igb_adapter *adapter, bool msix)
{
	//IOPCIDevice *pdev = adapter->pdev;
	//int err;
	int numvecs;

	if (!msix)
		adapter->int_mode = IGB_INT_MODE_MSI;

	/* Number of supported queues. */
	adapter->num_rx_queues = adapter->rss_queues;
	
	if (adapter->vmdq_pools > 1)
		adapter->num_rx_queues += adapter->vmdq_pools - 1;
	
#ifdef HAVE_TX_MQ
	if (adapter->vmdq_pools)
		adapter->num_tx_queues = adapter->vmdq_pools;
	else
		adapter->num_tx_queues = adapter->num_rx_queues;
#else
	adapter->num_tx_queues = max_t(u32, 1, adapter->vmdq_pools);
#endif
	
	switch (adapter->int_mode) {
		case IGB_INT_MODE_MSIX:
			/* start with one vector for every Tx/Rx queue */
			numvecs = max_t(int, adapter->num_tx_queues,
							adapter->num_rx_queues);
			
			/* if tx handler is separate make it 1 for every queue */
			if (!(adapter->flags & IGB_FLAG_QUEUE_PAIRS))
				numvecs = adapter->num_tx_queues +
				adapter->num_rx_queues;
			
			/* store the number of vectors reserved for queues */
			adapter->num_q_vectors = numvecs;
			
			/* add 1 vector for link status interrupts */
			numvecs++;
#ifndef __APPLE__
			adapter->msix_entries = (msix_entry*)kcalloc(numvecs, sizeof(struct msix_entry));
			if (adapter->msix_entries) {
				for (i = 0; i < numvecs; i++)
					adapter->msix_entries[i].entry = i;

				err = pci_enable_msix(pdev,
									  adapter->msix_entries, numvecs);
				if (err == 0)
					break;
			}
			/* MSI-X failed, so fall through and try MSI */
			dev_warn(pci_dev_to_dev(pdev), "Failed to initialize MSI-X interrupts. "
					 "Falling back to MSI interrupts.\n");
#endif
			igb_reset_interrupt_capability(adapter);
		case IGB_INT_MODE_MSI:
#ifndef __APPLE__
			if (!pci_enable_msi(pdev))
				adapter->flags |= IGB_FLAG_HAS_MSI;
			else
				dev_warn(pci_dev_to_dev(pdev), "Failed to initialize MSI "
						 "interrupts.  Falling back to legacy interrupts.\n");
#endif
			/* Fall through */
		case IGB_INT_MODE_LEGACY:
			/* disable advanced features and set number of queues to 1 */
			igb_reset_sriov_capability(adapter);
			adapter->vmdq_pools = 0;
			adapter->rss_queues = 1;
			adapter->flags |= IGB_FLAG_QUEUE_PAIRS;
			adapter->num_rx_queues = 1;
			adapter->num_tx_queues = 1;
			adapter->num_q_vectors = 1;
			/* Don't do anything; this is system default */
			break;
	}
	
#ifdef HAVE_TX_MQ
	/* Notify the stack of the (possibly) reduced Tx Queue count. */
#ifdef CONFIG_NETDEVICES_MULTIQUEUE
	adapter->netdev->egress_subqueue_count = adapter->num_tx_queues;
#else
	adapter->netdev->real_num_tx_queues =
	(adapter->vmdq_pools ? 1 : adapter->num_tx_queues);
#endif /* CONFIG_NETDEVICES_MULTIQUEUE */
#endif /* HAVE_TX_MQ */
}

static void igb_add_ring(struct igb_ring *ring,
						 struct igb_ring_container *head)
{
	head->ring = ring;
	head->count++;
}

/**
 * igb_alloc_q_vector - Allocate memory for a single interrupt vector
 * @adapter: board private structure to initialize
 * @v_count: q_vectors allocated on adapter, used for ring interleaving
 * @v_idx: index of vector in adapter struct
 * @txr_count: total number of Tx rings to allocate
 * @txr_idx: index of first Tx ring to allocate
 * @rxr_count: total number of Rx rings to allocate
 * @rxr_idx: index of first Rx ring to allocate
 *
 * We allocate one q_vector.  If allocation fails we return -ENOMEM.
 **/
static int igb_alloc_q_vector(struct igb_adapter *adapter,
							  unsigned int v_count, unsigned int v_idx,
							  unsigned int txr_count, unsigned int txr_idx,
							  unsigned int rxr_count, unsigned int rxr_idx)
{
	struct igb_q_vector *q_vector;
	struct igb_ring *ring;
	int ring_count, size;
	
	/* igb only supports 1 Tx and/or 1 Rx queue per vector */
	if (txr_count > 1 || rxr_count > 1)
		return -ENOMEM;
	
	ring_count = txr_count + rxr_count;
	size = sizeof(struct igb_q_vector) +
	(sizeof(struct igb_ring) * ring_count);
	
	/* allocate q_vector and rings */
	q_vector = adapter->q_vector[v_idx];
	if (!q_vector)
		q_vector = (igb_q_vector*)kzalloc(size);
	else
		memset(q_vector, 0, size);
	if (!q_vector)
		return -ENOMEM;
#ifdef	__APPLE__
	q_vector->alloc_size = size;
#endif
#ifndef IGB_NO_LRO
	/* initialize LRO */
	__skb_queue_head_init(&q_vector->lrolist.active);
	
#endif
	/* initialize NAPI */
#ifndef __APPLE__
	netif_napi_add(adapter->netdev, &q_vector->napi,
				   igb_poll, 64);
#endif
	/* tie q_vector and adapter together */
	adapter->q_vector[v_idx] = q_vector;
#ifdef __APPLE__
	adapter->q_vector_size[v_idx] = size;
#endif
	q_vector->adapter = adapter;
	
	/* initialize work limits */
	q_vector->tx.work_limit = adapter->tx_work_limit;
	
	/* initialize ITR configuration */
	q_vector->itr_register = adapter->io_addr + E1000_EITR(0);
	q_vector->itr_val = IGB_START_ITR;
	
	/* initialize pointer to rings */
	ring = q_vector->ring;
	
	/* intialize ITR */
	if (rxr_count) {
		/* rx or rx/tx vector */
		if (!adapter->rx_itr_setting || adapter->rx_itr_setting > 3)
			q_vector->itr_val = adapter->rx_itr_setting;
	} else {
		/* tx only vector */
		if (!adapter->tx_itr_setting || adapter->tx_itr_setting > 3)
			q_vector->itr_val = adapter->tx_itr_setting;
	}
	
	if (txr_count) {
		/* assign generic ring traits */
		//ring->dev = adapter->netdev->pdev;
		ring->netdev = adapter->netdev;
		
		/* configure backlink on ring */
		ring->q_vector = q_vector;
		
		/* update q_vector Tx values */
		igb_add_ring(ring, &q_vector->tx);
		
		/* For 82575, context index must be unique per ring. */
		if (adapter->hw.mac.type == e1000_82575)
			set_bit(IGB_RING_FLAG_TX_CTX_IDX, &ring->flags);
		
		/* apply Tx specific ring traits */
		ring->count = adapter->tx_ring_count;
		ring->queue_index = txr_idx;
		
		/* assign ring to adapter */
		adapter->tx_ring[txr_idx] = ring;
		
		/* push pointer to next ring */
		ring++;
	}
	
	if (rxr_count) {
		/* assign generic ring traits */
		//ring->dev = adapter->netdev->dev;
		ring->netdev = adapter->netdev;
		
		/* configure backlink on ring */
		ring->q_vector = q_vector;
		
		/* update q_vector Rx values */
		igb_add_ring(ring, &q_vector->rx);
		
#if defined(HAVE_RHEL6_NET_DEVICE_OPS_EXT) || !defined(HAVE_NDO_SET_FEATURES)
		/* enable rx checksum */
		set_bit(IGB_RING_FLAG_RX_CSUM, &ring->flags);
		
#endif
		/* set flag indicating ring supports SCTP checksum offload */
		if (adapter->hw.mac.type >= e1000_82576)
			set_bit(IGB_RING_FLAG_RX_SCTP_CSUM, &ring->flags);
		
		/* On i350, loopback VLAN packets have the tag byte-swapped */
		if ((adapter->hw.mac.type == e1000_i350) ||
		    (adapter->hw.mac.type == e1000_i354))
			set_bit(IGB_RING_FLAG_RX_LB_VLAN_BSWAP, &ring->flags);
		
		/* apply Rx specific ring traits */
		ring->count = adapter->rx_ring_count;
		ring->queue_index = rxr_idx;
		
		/* assign ring to adapter */
		adapter->rx_ring[rxr_idx] = ring;
	}
	
	return 0;
}

/**
 * igb_alloc_q_vectors - Allocate memory for interrupt vectors
 * @adapter: board private structure to initialize
 *
 * We allocate one q_vector per queue interrupt.  If allocation fails we
 * return -ENOMEM.
 **/
static int igb_alloc_q_vectors(struct igb_adapter *adapter)
{
	int q_vectors = adapter->num_q_vectors;
	int rxr_remaining = adapter->num_rx_queues;
	int txr_remaining = adapter->num_tx_queues;
	int rxr_idx = 0, txr_idx = 0, v_idx = 0;
	int err;
	
	if (q_vectors >= (rxr_remaining + txr_remaining)) {
		for (; rxr_remaining; v_idx++) {
			err = igb_alloc_q_vector(adapter, q_vectors, v_idx,
									 0, 0, 1, rxr_idx);
			
			if (err)
				goto err_out;
			
			/* update counts and index */
			rxr_remaining--;
			rxr_idx++;
		}
	}
	
	for (; v_idx < q_vectors; v_idx++) {
		int rqpv = DIV_ROUND_UP(rxr_remaining, q_vectors - v_idx);
		int tqpv = DIV_ROUND_UP(txr_remaining, q_vectors - v_idx);

		err = igb_alloc_q_vector(adapter, q_vectors, v_idx,
								 tqpv, txr_idx, rqpv, rxr_idx);

		if (err)
			goto err_out;
		
		/* update counts and index */
		rxr_remaining -= rqpv;
		txr_remaining -= tqpv;
		rxr_idx++;
		txr_idx++;
	}
	
	return 0;
	
err_out:
	adapter->num_tx_queues = 0;
	adapter->num_rx_queues = 0;
	adapter->num_q_vectors = 0;
	
	while (v_idx--)
		igb_free_q_vector(adapter, v_idx);
	
	return -ENOMEM;
}


/**
 * igb_init_interrupt_scheme - initialize interrupts, allocate queues/vectors
 *
 * This function initializes the interrupts and allocates all of the queues.
 **/
static int igb_init_interrupt_scheme(struct igb_adapter *adapter, bool msix)
{
	//IOPCIDevice *pdev = adapter->pdev;
	int err;
	
	igb_set_interrupt_capability(adapter, msix);
	
	err = igb_alloc_q_vectors(adapter);
	if (err) {
		pr_err("Unable to allocate memory for vectors\n");
		goto err_alloc_q_vectors;
	}
	
	igb_cache_ring_register(adapter);
	
	return 0;

err_alloc_q_vectors:
	igb_reset_interrupt_capability(adapter);
	return err;
}

/**
 * igb_request_irq - initialize interrupts
 *
 * Attempts to configure interrupts using the best available
 * capabilities of the hardware and kernel.
 **/
static int igb_request_irq(struct igb_adapter *adapter)
{
	int err = 0;
	
	if (adapter->msix_entries) {
		err = igb_request_msix(adapter);
		if (!err)
			goto request_done;
		/* fall back to MSI */
		igb_free_all_tx_resources(adapter);
		igb_free_all_rx_resources(adapter);

		igb_clear_interrupt_scheme(adapter);
		igb_reset_sriov_capability(adapter);
		err = igb_init_interrupt_scheme(adapter, false);
		if (err)
			goto request_done;

		igb_setup_all_tx_resources(adapter);
		igb_setup_all_rx_resources(adapter);
		igb_configure(adapter);
	}
	
	igb_assign_vector(adapter->q_vector[0], 0);
	
#ifndef __APPLE__
	if (adapter->flags & IGB_FLAG_HAS_MSI) {
		err = request_irq(pdev->irq, &igb_intr_msi, 0,
						  netdev->name, adapter);
		if (!err)
			goto request_done;
		
		/* fall back to legacy interrupts */
		igb_reset_interrupt_capability(adapter);
		adapter->flags &= ~IGB_FLAG_HAS_MSI;
	}
	err = request_irq(pdev->irq, &igb_intr, IRQF_SHARED,
					  netdev->name, adapter);
#endif	
	
	if (err)
		pr_err("Error %d getting interrupt\n", err);
	
request_done:
	return err;
}

static void igb_free_irq(struct igb_adapter *adapter)
{
#ifndef __APPLE__
	if (adapter->msix_entries) {
		int vector = 0, i;
		
		free_irq(adapter->msix_entries[vector++].vector, adapter);
		
		for (i = 0; i < adapter->num_q_vectors; i++)
			free_irq(adapter->msix_entries[vector++].vector,
			         adapter->q_vector[i]);
	} else {
		free_irq(adapter->pdev->irq, adapter);
	}
#endif
}

/**
 * igb_irq_disable - Mask off interrupt generation on the NIC
 * @adapter: board private structure
 **/
static void igb_irq_disable(struct igb_adapter *adapter)
{
	struct e1000_hw *hw = &adapter->hw;
	
	E1000_WRITE_REG(hw, E1000_IMC, ~0);
	E1000_WRITE_FLUSH(hw);
}

/**
 * igb_irq_enable - Enable default interrupt generation settings
 * @adapter: board private structure
 **/
static void igb_irq_enable(struct igb_adapter *adapter)
{
	struct e1000_hw *hw = &adapter->hw;
	
	E1000_WRITE_REG(hw, E1000_IMS, IMS_ENABLE_MASK | E1000_IMS_DRSTA);
    E1000_WRITE_REG(hw, E1000_IAM, IMS_ENABLE_MASK | E1000_IMS_DRSTA);
}
	
/**
 * igb_release_hw_control - release control of the h/w to f/w
 * @adapter: address of board private structure
 *
 * igb_release_hw_control resets CTRL_EXT:DRV_LOAD bit.
 * For ASF and Pass Through versions of f/w this means that the
 * driver is no longer loaded.
 *
 **/
static void igb_release_hw_control(struct igb_adapter *adapter)
{
    struct e1000_hw *hw = &adapter->hw;
    u32 ctrl_ext;
    
    /* Let firmware take over control of h/w */
    ctrl_ext = E1000_READ_REG(hw, E1000_CTRL_EXT);
    E1000_WRITE_REG(hw, E1000_CTRL_EXT,
                    ctrl_ext & ~E1000_CTRL_EXT_DRV_LOAD);
}
	
/**
 * igb_get_hw_control - get control of the h/w from f/w
 * @adapter: address of board private structure
 *
 * igb_get_hw_control sets CTRL_EXT:DRV_LOAD bit.
 * For ASF and Pass Through versions of f/w this means that
 * the driver is loaded.
 *
 **/
static void igb_get_hw_control(struct igb_adapter *adapter)
{
    struct e1000_hw *hw = &adapter->hw;
    u32 ctrl_ext;
    
    /* Let firmware know the driver has taken over */
    ctrl_ext = E1000_READ_REG(hw, E1000_CTRL_EXT);
    E1000_WRITE_REG(hw, E1000_CTRL_EXT,
                    ctrl_ext | E1000_CTRL_EXT_DRV_LOAD);
}
	
/**
 * igb_configure - configure the hardware for RX and TX
 * @adapter: private board structure
 **/
static void igb_configure(struct igb_adapter *adapter)
{
    IOEthernetController* netdev = adapter->netdev;
    int i;
    
    igb_get_hw_control(adapter);
    igb_set_rx_mode(netdev);
    
    igb_restore_vlan(adapter);
    
    igb_setup_tctl(adapter);
    igb_setup_mrqc(adapter);
    igb_setup_rctl(adapter);
    
    igb_configure_tx(adapter);
    igb_configure_rx(adapter);
    
    e1000_rx_fifo_flush_82575(&adapter->hw);
#ifdef CONFIG_NETDEVICES_MULTIQUEUE
    if (adapter->num_tx_queues > 1)
        netdev->features |= NETIF_F_MULTI_QUEUE;
    else
        netdev->features &= ~NETIF_F_MULTI_QUEUE;
#endif
    
    /* call igb_desc_unused which always leaves
     * at least 1 descriptor unused to make sure
     * next_to_use != next_to_clean
	 */
    for (i = 0; i < adapter->num_rx_queues; i++) {
        struct igb_ring *ring = adapter->rx_ring[i];
        igb_alloc_rx_buffers(ring, igb_desc_unused(ring));
    }
}
	
/**
 * igb_power_up_link - Power up the phy/serdes link
 * @adapter: address of board private structure
 **/
void igb_power_up_link(struct igb_adapter *adapter)
{
	e1000_phy_hw_reset(&adapter->hw);
	
	if (adapter->hw.phy.media_type == e1000_media_type_copper)
		e1000_power_up_phy(&adapter->hw);
	else
		e1000_power_up_fiber_serdes_link(&adapter->hw);
}
	
/**
 * igb_power_down_link - Power down the phy/serdes link
 * @adapter: address of board private structure
 */
static void igb_power_down_link(struct igb_adapter *adapter)
{
	if (adapter->hw.phy.media_type == e1000_media_type_copper)
		e1000_power_down_phy(&adapter->hw);
	else
		e1000_shutdown_fiber_serdes_link(&adapter->hw);
}

	
/* Detect and switch function for Media Auto Sense */
static void igb_check_swap_media(struct igb_adapter *adapter)
{
    struct e1000_hw *hw = &adapter->hw;
    u32 ctrl_ext, connsw;
    bool swap_now = false;
    bool link;
    
    ctrl_ext = E1000_READ_REG(hw, E1000_CTRL_EXT);
    connsw = E1000_READ_REG(hw, E1000_CONNSW);
    link = igb_has_link(adapter);
    
    /* need to live swap if current media is copper and we have fiber/serdes
     * to go to.
     */
    
    if ((hw->phy.media_type == e1000_media_type_copper) &&
        (!(connsw & E1000_CONNSW_AUTOSENSE_EN))) {
        swap_now = true;
    } else if (!(connsw & E1000_CONNSW_SERDESD)) {
        /* copper signal takes time to appear */
        if (adapter->copper_tries < 3) {
            adapter->copper_tries++;
            connsw |= E1000_CONNSW_AUTOSENSE_CONF;
            E1000_WRITE_REG(hw, E1000_CONNSW, connsw);
            return;
        } else {
            adapter->copper_tries = 0;
            if ((connsw & E1000_CONNSW_PHYSD) &&
                (!(connsw & E1000_CONNSW_PHY_PDN))) {
                swap_now = true;
                connsw &= ~E1000_CONNSW_AUTOSENSE_CONF;
                E1000_WRITE_REG(hw, E1000_CONNSW, connsw);
            }
        }
    }
    
    if (swap_now) {
        switch (hw->phy.media_type) {
            case e1000_media_type_copper:
                pr_err( "%s:MAS: changing media to fiber/serdes",
                         "AppleIGB\n");
                ctrl_ext |=
                E1000_CTRL_EXT_LINK_MODE_PCIE_SERDES;
                adapter->flags |= IGB_FLAG_MEDIA_RESET;
                adapter->copper_tries = 0;
                break;
            case e1000_media_type_internal_serdes:
            case e1000_media_type_fiber:
                pr_err("%s:MAS: changing media to copper",
                         "AppleIGB\n");
                ctrl_ext &=
                ~E1000_CTRL_EXT_LINK_MODE_PCIE_SERDES;
                adapter->flags |= IGB_FLAG_MEDIA_RESET;
                break;
            default:
                /* shouldn't get here during regular operation */
                pr_err("%s:AMS: Invalid media type found, returning",
                        "AppleIGB\n");
                break;
        }
        E1000_WRITE_REG(hw, E1000_CTRL_EXT, ctrl_ext);
    }
}
    
#ifdef HAVE_I2C_SUPPORT
/*  igb_get_i2c_data - Reads the I2C SDA data bit
 *  @hw: pointer to hardware structure
 *  @i2cctl: Current value of I2CCTL register
 *
 *  Returns the I2C data bit value
 */
static int igb_get_i2c_data(void *data)
{
	struct igb_adapter *adapter = (struct igb_adapter *)data;
	struct e1000_hw *hw = &adapter->hw;
	s32 i2cctl = E1000_READ_REG(hw, E1000_I2CPARAMS);
	
	return !!(i2cctl & E1000_I2C_DATA_IN);
}

/* igb_set_i2c_data - Sets the I2C data bit
 *  @data: pointer to hardware structure
 *  @state: I2C data value (0 or 1) to set
 *
 *  Sets the I2C data bit
 */
static void igb_set_i2c_data(void *data, int state)
{
	struct igb_adapter *adapter = (struct igb_adapter *)data;
	struct e1000_hw *hw = &adapter->hw;
	s32 i2cctl = E1000_READ_REG(hw, E1000_I2CPARAMS);
	
	if (state)
		i2cctl |= E1000_I2C_DATA_OUT;
	else
		i2cctl &= ~E1000_I2C_DATA_OUT;
	
	i2cctl &= ~E1000_I2C_DATA_OE_N;
	i2cctl |= E1000_I2C_CLK_OE_N;
	
	E1000_WRITE_REG(hw, E1000_I2CPARAMS, i2cctl);
	E1000_WRITE_FLUSH(hw);
	
}

/* igb_set_i2c_clk - Sets the I2C SCL clock
 *  @data: pointer to hardware structure
 *  @state: state to set clock
 *
 *  Sets the I2C clock line to state
 */
static void igb_set_i2c_clk(void *data, int state)
{
	struct igb_adapter *adapter = (struct igb_adapter *)data;
	struct e1000_hw *hw = &adapter->hw;
	s32 i2cctl = E1000_READ_REG(hw, E1000_I2CPARAMS);
	
	if (state) {
		i2cctl |= E1000_I2C_CLK_OUT;
		i2cctl &= ~E1000_I2C_CLK_OE_N;
	} else {
		i2cctl &= ~E1000_I2C_CLK_OUT;
		i2cctl &= ~E1000_I2C_CLK_OE_N;
	}
	E1000_WRITE_REG(hw, E1000_I2CPARAMS, i2cctl);
	E1000_WRITE_FLUSH(hw);
}

/* igb_get_i2c_clk - Gets the I2C SCL clock state
 *  @data: pointer to hardware structure
 *
 *  Gets the I2C clock state
 */
static int igb_get_i2c_clk(void *data)
{
	struct igb_adapter *adapter = (struct igb_adapter *)data;
	struct e1000_hw *hw = &adapter->hw;
	s32 i2cctl = E1000_READ_REG(hw, E1000_I2CPARAMS);
	
	return ((i2cctl & E1000_I2C_CLK_IN) != 0);
}

static const struct i2c_algo_bit_data igb_i2c_algo = {
	.setsda		= igb_set_i2c_data,
	.setscl		= igb_set_i2c_clk,
	.getsda		= igb_get_i2c_data,
	.getscl		= igb_get_i2c_clk,
	.udelay		= 5,
	.timeout	= 20,
};

/*  igb_init_i2c - Init I2C interface
 *  @adapter: pointer to adapter structure
 *
 */
static s32 igb_init_i2c(struct igb_adapter *adapter)
{
	s32 status = E1000_SUCCESS;
	
	/* I2C interface supported on i350 devices */
	if (adapter->hw.mac.type != e1000_i350)
		return E1000_SUCCESS;
	
	/* Initialize the i2c bus which is controlled by the registers.
	 * This bus will use the i2c_algo_bit structue that implements
	 * the protocol through toggling of the 4 bits in the register.
	 */
	adapter->i2c_adap.owner = THIS_MODULE;
	adapter->i2c_algo = igb_i2c_algo;
	adapter->i2c_algo.data = adapter;
	adapter->i2c_adap.algo_data = &adapter->i2c_algo;
	adapter->i2c_adap.dev.parent = &adapter->pdev->dev;
	strlcpy(adapter->i2c_adap.name, "igb BB",
			sizeof(adapter->i2c_adap.name));
	status = i2c_bit_add_bus(&adapter->i2c_adap);
	return status;
}
#endif /* HAVE_I2C_SUPPORT */

/**
 * igb_up - Open the interface and prepare it to handle traffic
 * @adapter: board private structure
 **/
int igb_up(struct igb_adapter *adapter)
{
	struct e1000_hw *hw = &adapter->hw;
	//int i;
	
	/* hardware has been reset, we need to reload some things */
	igb_configure(adapter);
	
	clear_bit(__IGB_DOWN, &adapter->state);
#ifndef __APPLE__
	for (i = 0; i < adapter->num_q_vectors; i++)
		napi_enable(&(adapter->q_vector[i]->napi));
#endif
	if (adapter->msix_entries)
		igb_configure_msix(adapter);
	else
		igb_assign_vector(adapter->q_vector[0], 0);
	
	igb_configure_lli(adapter);
	
	/* Clear any pending interrupts. */
	E1000_READ_REG(hw, E1000_ICR);
	igb_irq_enable(adapter);
	
	/* notify VFs that reset has been completed */
	if (adapter->vfs_allocated_count) {
		u32 reg_data = E1000_READ_REG(hw, E1000_CTRL_EXT);
		reg_data |= E1000_CTRL_EXT_PFRSTD;
		E1000_WRITE_REG(hw, E1000_CTRL_EXT, reg_data);
	}
	
	netif_tx_start_all_queues(adapter->netdev);
	
	if (adapter->flags & IGB_FLAG_DETECT_BAD_DMA)
		schedule_work(&adapter->dma_err_task);
	/* start the watchdog. */
	hw->mac.get_link_status = 1;
#ifdef __APPLE__
    adapter->netdev->setTimers(TRUE);
#else
	schedule_work(&adapter->watchdog_task);
#endif
	if ((adapter->flags & IGB_FLAG_EEE) &&
	    (!hw->dev_spec._82575.eee_disable))
		adapter->eee_advert = MDIO_EEE_100TX | MDIO_EEE_1000T;
    
	return 0;
}
	
void igb_down(struct igb_adapter *adapter)
{
	IOEthernetController* netdev = adapter->netdev;
	struct e1000_hw *hw = &adapter->hw;
	u32 tctl, rctl;
	//int i;
	
	/* signal that we're down so the interrupt handler does not
	 * reschedule our watchdog timer
	 */
	set_bit(__IGB_DOWN, &adapter->state);
	
	/* disable receives in the hardware */
	rctl = E1000_READ_REG(hw, E1000_RCTL);
	E1000_WRITE_REG(hw, E1000_RCTL, rctl & ~E1000_RCTL_EN);
	/* flush and sleep below */

	netif_carrier_off(netdev);
	netif_tx_stop_all_queues(netdev);

	/* disable transmits in the hardware */
	tctl = E1000_READ_REG(hw, E1000_TCTL);
	tctl &= ~E1000_TCTL_EN;
	E1000_WRITE_REG(hw, E1000_TCTL, tctl);
	/* flush both disables and wait for them to finish */
	E1000_WRITE_FLUSH(hw);
	usleep_range(10000, 20000);

#ifndef __APPLE__
	for (i = 0; i < adapter->num_q_vectors; i++)
		napi_disable(&(adapter->q_vector[i]->napi));
#endif
	igb_irq_disable(adapter);

	adapter->flags &= ~IGB_FLAG_NEED_LINK_UPDATE;

#ifdef __APPLE__
    adapter->netdev->setTimers(FALSE);
#else
	del_timer_sync(&adapter->watchdog_timer);
	if (adapter->flags & IGB_FLAG_DETECT_BAD_DMA)
		del_timer_sync(&adapter->dma_err_timer);
	del_timer_sync(&adapter->phy_info_timer);
#endif

	/* record the stats before reset*/
	igb_update_stats(adapter);
	
	adapter->link_speed = 0;
	adapter->link_duplex = 0;
	
#ifdef HAVE_PCI_ERS
	if (!pci_channel_offline(adapter->pdev))
		igb_reset(adapter);
#else
	igb_reset(adapter);
#endif
	igb_clean_all_tx_rings(adapter);
	igb_clean_all_rx_rings(adapter);
#ifdef IGB_DCA

	/* since we reset the hardware DCA settings were cleared */
	igb_setup_dca(adapter);
#endif
}
	
void igb_reinit_locked(struct igb_adapter *adapter)
{
	WARN_ON(in_interrupt());
	while (test_and_set_bit(__IGB_RESETTING, &adapter->state))
		usleep_range(1000, 2000);
	igb_down(adapter);
	igb_up(adapter);
	clear_bit(__IGB_RESETTING, &adapter->state);
}

/**
 * igb_enable_mas - Media Autosense re-enable after swap
 *
 * @adapter: adapter struct
 **/
static void igb_enable_mas(struct igb_adapter *adapter)
{
    struct e1000_hw *hw = &adapter->hw;
    u32 connsw;
    
    connsw = E1000_READ_REG(hw, E1000_CONNSW);
    if (hw->phy.media_type == e1000_media_type_copper) {
        if (!(connsw & E1000_CONNSW_SERDESD)) {
            connsw |= E1000_CONNSW_ENRGSRC;
            connsw |= E1000_CONNSW_AUTOSENSE_EN;
            E1000_WRITE_REG(hw, E1000_CONNSW, connsw);
            E1000_WRITE_FLUSH(hw);
        }
    }
}


void igb_reset(struct igb_adapter *adapter)
{
	//IOPCIDevice *pdev = adapter->pdev;
	struct e1000_hw *hw = &adapter->hw;
	struct e1000_mac_info *mac = &hw->mac;
	struct e1000_fc_info *fc = &hw->fc;
	u32 pba = 0, tx_space, min_tx_space, min_rx_space, hwm;
		
	/* Repartition Pba for greater than 9k mtu
	 * To take effect CTRL.RST is required.
	 */
	switch (mac->type) {
		case e1000_i350:
		case e1000_82580:
        case e1000_i354:
			pba = E1000_READ_REG(hw, E1000_RXPBS);
			pba = e1000_rxpbs_adjust_82580(pba);
			break;
		case e1000_82576:
			pba = E1000_READ_REG(hw, E1000_RXPBS);
			pba &= E1000_RXPBS_SIZE_MASK_82576;
			break;
		case e1000_82575:
		case e1000_i210:
		case e1000_i211:
		default:
			pba = E1000_PBA_34K;
			break;
	}
	
	if ((adapter->max_frame_size > ETH_FRAME_LEN + ETH_FCS_LEN) &&
		(mac->type < e1000_82576)) {
		/* adjust PBA for jumbo frames */
		E1000_WRITE_REG(hw, E1000_PBA, pba);
		
		/* To maintain wire speed transmits, the Tx FIFO should be
		 * large enough to accommodate two full transmit packets,
		 * rounded up to the next 1KB and expressed in KB.  Likewise,
		 * the Rx FIFO should be large enough to accommodate at least
		 * one full receive packet and is similarly rounded up and
		 * expressed in KB.
		 */
		pba = E1000_READ_REG(hw, E1000_PBA);
		/* upper 16 bits has Tx packet buffer allocation size in KB */
		tx_space = pba >> 16;
		/* lower 16 bits has Rx packet buffer allocation size in KB */
		pba &= 0xffff;
		/* the tx fifo also stores 16 bytes of information about the tx
		 * but don't include ethernet FCS because hardware appends it
		 */
		min_tx_space = (adapter->max_frame_size +
						sizeof(union e1000_adv_tx_desc) -
						ETH_FCS_LEN) * 2;
		min_tx_space = ALIGN(min_tx_space, 1024);
		min_tx_space >>= 10;
		/* software strips receive CRC, so leave room for it */
		min_rx_space = adapter->max_frame_size;
		min_rx_space = ALIGN(min_rx_space, 1024);
		min_rx_space >>= 10;
		
		/* If current Tx allocation is less than the min Tx FIFO size,
		 * and the min Tx FIFO size is less than the current Rx FIFO
		 * allocation, take space away from current Rx allocation
		 */
		if (tx_space < min_tx_space &&
			((min_tx_space - tx_space) < pba)) {
			pba = pba - (min_tx_space - tx_space);
			
			/* if short on rx space, rx wins and must trump tx
			 * adjustment */
			if (pba < min_rx_space)
				pba = min_rx_space;
		}
		E1000_WRITE_REG(hw, E1000_PBA, pba);
	}
	
	/* flow control settings */
	/* The high water mark must be low enough to fit one full frame
	 * (or the size used for early receive) above it in the Rx FIFO.
	 * Set it to the lower of:
	 * - 90% of the Rx FIFO size, or
	 * - the full Rx FIFO size minus one full frame
	 */
	hwm = min(((pba << 10) * 9 / 10),
			  ((pba << 10) - 2 * adapter->max_frame_size));
	
	fc->high_water = hwm & 0xFFFFFFF0;	/* 16-byte granularity */
	fc->low_water = fc->high_water - 16;
	fc->pause_time = 0xFFFF;
	fc->send_xon = 1;
	fc->current_mode = fc->requested_mode;
	
	/* disable receive for all VFs and wait one second */
	if (adapter->vfs_allocated_count) {
		int i;

		/*
		 * Clear all flags except indication that the PF has set
		 * the VF MAC addresses administratively
		 */
		for (i = 0 ; i < adapter->vfs_allocated_count; i++)
			adapter->vf_data[i].flags &= IGB_VF_FLAG_PF_SET_MAC;
		
		/* ping all the active vfs to let them know we are going down */
		igb_ping_all_vfs(adapter);
		
		/* disable transmits and receives */
		E1000_WRITE_REG(hw, E1000_VFRE, 0);
		E1000_WRITE_REG(hw, E1000_VFTE, 0);
	}
	
	/* Allow time for pending master requests to run */
	e1000_reset_hw(hw);
	E1000_WRITE_REG(hw, E1000_WUC, 0);
	
	if (adapter->flags & IGB_FLAG_MEDIA_RESET) {
		e1000_setup_init_funcs(hw, TRUE);
		igb_check_options(adapter);
		e1000_get_bus_info(hw);
		adapter->flags &= ~IGB_FLAG_MEDIA_RESET;
	}
	if ((mac->type == e1000_82575) &&
		(adapter->flags & IGB_FLAG_MAS_ENABLE)) {
		igb_enable_mas(adapter);
	}
	if (e1000_init_hw(hw))
		pr_err( "Hardware Error!\n");
	
	/*
	 * Flow control settings reset on hardware reset, so guarantee flow
	 * control is off when forcing speed.
	 */
	if (!hw->mac.autoneg)
		e1000_force_mac_fc(hw);
	
	igb_init_dmac(adapter, pba);
	/* Re-initialize the thermal sensor on i350 devices. */
	if (mac->type == e1000_i350 && hw->bus.func == 0) {
		/*
		 * If present, re-initialize the external thermal sensor
		 * interface.
		 */
		if (adapter->ets)
			e1000_set_i2c_bb(hw);
		e1000_init_thermal_sensor_thresh(hw);
	}

	/*Re-establish EEE setting */
	if (hw->phy.media_type == e1000_media_type_copper) {
		switch (mac->type) {
            case e1000_i350:
            case e1000_i210:
            case e1000_i211:
                e1000_set_eee_i350(hw, true, true);
                break;
            case e1000_i354:
                e1000_set_eee_i354(hw, true, true);
                break;
            default:
                break;
		}
	}

	if (!netif_running(adapter->netdev))
		igb_power_down_link(adapter);

	//igb_update_mng_vlan(adapter);

	/* Enable h/w to recognize an 802.1Q VLAN Ethernet packet */
	E1000_WRITE_REG(hw, E1000_VET, ETHERNET_IEEE_VLAN_TYPE);
#ifdef HAVE_PTP_1588_CLOCK
	/* Re-enable PTP, where applicable. */
	igb_ptp_reset(adapter);
#endif /* HAVE_PTP_1588_CLOCK */

	e1000_get_phy_info(hw);

	adapter->devrc++;
}

#ifdef HAVE_NDO_SET_FEATURES
#ifdef HAVE_RHEL6_NET_DEVICE_OPS_EXT
	static u32 igb_fix_features(struct net_device *netdev,
								u32 features)
#else
	static netdev_features_t igb_fix_features(struct net_device *netdev,
											  netdev_features_t features)
#endif /* HAVE_RHEL6_NET_DEVICE_OPS_EXT */
	{
	/*
	 * Since there is no support for separate tx vlan accel
	 * enabled make sure tx flag is cleared if rx is.
	 */
#ifdef NETIF_F_HW_VLAN_CTAG_RX
	if (!(features & NETIF_F_HW_VLAN_CTAG_RX))
		features &= ~NETIF_F_HW_VLAN_CTAG_TX;
#else
	if (!(features & NETIF_F_HW_VLAN_RX))
		features &= ~NETIF_F_HW_VLAN_TX;
#endif /* NETIF_F_HW_VLAN_CTAG_RX */
		
#ifndef IGB_NO_LRO
	/* If Rx checksum is disabled, then LRO should also be disabled */
	if (!(features & NETIF_F_RXCSUM))
		features &= ~NETIF_F_LRO;
	
#endif
	return features;
}
	
static int igb_set_features(struct net_device *netdev,
#ifdef HAVE_RHEL6_NET_DEVICE_OPS_EXT
							u32 features)
#else
	netdev_features_t features)
#endif /* HAVE_RHEL6_NET_DEVICE_OPS_EXT */
{
	netdev_features_t changed = netdev->features() ^ features;
#ifdef HAVE_RHEL6_NET_DEVICE_OPS_EXT
	struct igb_adapter *adapter = netdev_priv(netdev);
#endif
	
#ifdef NETIF_F_HW_VLAN_CTAG_RX
	if (changed & NETIF_F_HW_VLAN_CTAG_RX)
#else
    if (changed & NETIF_F_HW_VLAN_RX)
#endif /* NETIF_F_HW_VLAN_CTAG_RX */
		netdev->features = features;
#ifdef HAVE_VLAN_RX_REGISTER
	igb_vlan_mode(netdev, adapter->vlgrp);
#else
	igb_vlan_mode(netdev, features);
#endif
	
	if (!(changed & (NETIF_F_RXALL | NETIF_F_NTUPLE)))
		return 0;
	
	netdev->features = features;

	return 0;
}
#endif /* HAVE_NDO_SET_FEATURES */

#ifdef HAVE_FDB_OPS
#ifdef USE_CONST_DEV_UC_CHAR
static int igb_ndo_fdb_add(struct ndmsg *ndm, struct nlattr *tb[],
                struct net_device *dev,
				const unsigned char *addr,
#ifdef HAVE_NDO_FDB_ADD_VID
						   u16 vid,
#endif /* HAVE_NDO_FDB_ADD_VID */
						   u16 flags)
#else /* USE_CONST_DEV_UC_CHAR */
static int igb_ndo_fdb_add(struct ndmsg *ndm,
							   struct net_device *dev,
							   unsigned char *addr,
							   u16 flags)
#endif /* USE_CONST_DEV_UC_CHAR */
	{
    struct igb_adapter *adapter = netdev_priv(dev);
    struct e1000_hw *hw = &adapter->hw;
    int err;
    
    if (!(adapter->vfs_allocated_count))
        return -EOPNOTSUPP;
    
    /* Hardware does not support aging addresses so if a
     * ndm_state is given only allow permanent addresses
     */
    if (ndm->ndm_state && !(ndm->ndm_state & NUD_PERMANENT)) {
        pr_info("%s: FDB only supports static addresses\n",
                igb_driver_name);
        return -EINVAL;
    }
    
    if (is_unicast_ether_addr(addr) || is_link_local_ether_addr(addr)) {
        u32 rar_uc_entries = hw->mac.rar_entry_count -
        (adapter->vfs_allocated_count + 1);
        
        if (netdev_uc_count(dev) < rar_uc_entries)
            err = dev_uc_add_excl(dev, addr);
        else
            err = -ENOMEM;
    } else if (is_multicast_ether_addr(addr)) {
        err = dev_mc_add_excl(dev, addr);
    } else {
        err = -EINVAL;
    }
    
    /* Only return duplicate errors if NLM_F_EXCL is set */
    if (err == -EEXIST && !(flags & NLM_F_EXCL))
        err = 0;
    
    return err;
	}
	
#ifndef USE_DEFAULT_FDB_DEL_DUMP
#ifdef USE_CONST_DEV_UC_CHAR
static int igb_ndo_fdb_del(struct ndmsg *ndm,
							   struct net_device *dev,
							   const unsigned char *addr)
#else
static int igb_ndo_fdb_del(struct ndmsg *ndm,
							   struct net_device *dev,
							   unsigned char *addr)
#endif /* USE_CONST_DEV_UC_CHAR */
{
    struct igb_adapter *adapter = netdev_priv(dev);
    int err = -EOPNOTSUPP;
    
    if (ndm->ndm_state & NUD_PERMANENT) {
        pr_info("%s: FDB only supports static addresses\n",
                igb_driver_name);
        return -EINVAL;
    }
    
    if (adapter->vfs_allocated_count) {
        if (is_unicast_ether_addr(addr))
            err = dev_uc_del(dev, addr);
        else if (is_multicast_ether_addr(addr))
            err = dev_mc_del(dev, addr);
        else
            err = -EINVAL;
    }
    
    return err;
}
	
static int igb_ndo_fdb_dump(struct sk_buff *skb,
                            struct netlink_callback *cb,
                            struct net_device *dev,
                            int idx)
{
    struct igb_adapter *adapter = netdev_priv(dev);
    
    if (adapter->vfs_allocated_count)
        idx = ndo_dflt_fdb_dump(skb, cb, dev, idx);
    
    return idx;
}
#endif /* USE_DEFAULT_FDB_DEL_DUMP */
    
#ifdef HAVE_BRIDGE_ATTRIBS
static int igb_ndo_bridge_setlink(struct net_device *dev,
                                  struct nlmsghdr *nlh)
{
    struct igb_adapter *adapter = netdev_priv(dev);
    struct e1000_hw *hw = &adapter->hw;
    struct nlattr *attr, *br_spec;
    int rem;
    
    if (!(adapter->vfs_allocated_count))
        return -EOPNOTSUPP;
    
    switch (adapter->hw.mac.type) {
        case e1000_82576:
        case e1000_i350:
        case e1000_i354:
            break;
        default:
            return -EOPNOTSUPP;
    }
    
    br_spec = nlmsg_find_attr(nlh, sizeof(struct ifinfomsg), IFLA_AF_SPEC);
    
    nla_for_each_nested(attr, br_spec, rem) {
        __u16 mode;
        
        if (nla_type(attr) != IFLA_BRIDGE_MODE)
            continue;
        
        mode = nla_get_u16(attr);
        if (mode == BRIDGE_MODE_VEPA) {
            e1000_vmdq_set_loopback_pf(hw, 0);
            adapter->flags &= ~IGB_FLAG_LOOPBACK_ENABLE;
        } else if (mode == BRIDGE_MODE_VEB) {
            e1000_vmdq_set_loopback_pf(hw, 1);
            adapter->flags |= IGB_FLAG_LOOPBACK_ENABLE;
        } else
            return -EINVAL;
        
        netdev_info(adapter->netdev, "enabling bridge mode: %s\n",
                    mode == BRIDGE_MODE_VEPA ? "VEPA" : "VEB");
    }
    
    return 0;
}

#ifdef HAVE_NDO_BRIDGE_GETLINK_NLFLAGS
	static int igb_ndo_bridge_getlink(struct sk_buff *skb, u32 pid, u32 seq,
									  struct net_device *dev, u32 filter_mask,
									  int nlflags)
#elif defined(HAVE_BRIDGE_FILTER)
	static int igb_ndo_bridge_getlink(struct sk_buff *skb, u32 pid, u32 seq,
									  struct net_device *dev, u32 filter_mask)
#else
	static int igb_ndo_bridge_getlink(struct sk_buff *skb, u32 pid, u32 seq,
									  struct net_device *dev)
#endif /* HAVE_NDO_BRIDGE_GETLINK_NLFLAGS */
{
    struct igb_adapter *adapter = netdev_priv(dev);
    u16 mode;
    
    if (!(adapter->vfs_allocated_count))
        return -EOPNOTSUPP;
    
    if (adapter->flags & IGB_FLAG_LOOPBACK_ENABLE)
        mode = BRIDGE_MODE_VEB;
    else
        mode = BRIDGE_MODE_VEPA;
#ifdef HAVE_NDO_DFLT_BRIDGE_GETLINK_VLAN_SUPPORT
	return ndo_dflt_bridge_getlink(skb, pid, seq, dev, mode, 0, 0, nlflags,
								   filter_mask, NULL);
#elif defined(HAVE_NDO_BRIDGE_GETLINK_NLFLAGS)
	return ndo_dflt_bridge_getlink(skb, pid, seq, dev, mode, 0, 0, nlflags);
#elif defined(NDO_DFLT_BRIDGE_GETLINK_HAS_BRFLAGS)
	return ndo_dflt_bridge_getlink(skb, pid, seq, dev, mode, 0, 0);
#else
	return ndo_dflt_bridge_getlink(skb, pid, seq, dev, mode);
#endif /* NDO_DFLT_BRIDGE_GETLINK_HAS_BRFLAGS */
}
#endif /* HAVE_BRIDGE_ATTRIBS */
#endif /* HAVE_FDB_OPS */


static void igb_set_fw_version(struct igb_adapter *adapter)
{
	struct e1000_hw *hw = &adapter->hw;
	struct e1000_fw_version fw;
	
	e1000_get_fw_version(hw, &fw);
	
	switch (hw->mac.type) {
    case e1000_i210:
	case e1000_i211:
        if (!(e1000_get_flash_presence_i210(hw))) {
            snprintf(adapter->fw_version,
                     sizeof(adapter->fw_version),
                     "%2d.%2d-%d",
                     fw.invm_major, fw.invm_minor, fw.invm_img_type);
            break;
        }
        /* fall through */
	default:
		/* if option rom is valid, display its version too*/
		if (fw.or_valid) {
			snprintf(adapter->fw_version,
					 sizeof(adapter->fw_version),
					 "%d.%d, 0x%08x, %d.%d.%d",
					 fw.eep_major, fw.eep_minor, fw.etrack_id,
					 fw.or_major, fw.or_build, fw.or_patch);
			/* no option rom */
		} else {
			if (fw.etrack_id != 0X0000) {
				snprintf(adapter->fw_version,
						 sizeof(adapter->fw_version),
						 "%d.%d, 0x%08x",
						 fw.eep_major, fw.eep_minor, fw.etrack_id);
			} else {
				snprintf(adapter->fw_version,
						 sizeof(adapter->fw_version),
						 "%d.%d.%d",
						 fw.eep_major, fw.eep_minor, fw.eep_build);
			}
		}
		break;
	}
}
	

/**
 * igb_init_mas - init Media Autosense feature if enabled in the NVM
 *
 * @adapter: adapter struct
 **/
static void igb_init_mas(struct igb_adapter *adapter)
{
    struct e1000_hw *hw = &adapter->hw;
    u16 eeprom_data;
    
    e1000_read_nvm(hw, NVM_COMPAT, 1, &eeprom_data);
    switch (hw->bus.func) {
        case E1000_FUNC_0:
            if (eeprom_data & IGB_MAS_ENABLE_0)
                adapter->flags |= IGB_FLAG_MAS_ENABLE;
            break;
        case E1000_FUNC_1:
            if (eeprom_data & IGB_MAS_ENABLE_1)
                adapter->flags |= IGB_FLAG_MAS_ENABLE;
            break;
        case E1000_FUNC_2:
            if (eeprom_data & IGB_MAS_ENABLE_2)
                adapter->flags |= IGB_FLAG_MAS_ENABLE;
            break;
        case E1000_FUNC_3:
            if (eeprom_data & IGB_MAS_ENABLE_3)
                adapter->flags |= IGB_FLAG_MAS_ENABLE;
            break;
        default:
            /* Shouldn't get here */
            pr_err("%s:AMS: Invalid port configuration, returning\n",
                    "AppleIGB\n");
            break;
    }
}

void igb_rar_set(struct igb_adapter *adapter, u32 index)
{
	u32 rar_low, rar_high;
	struct e1000_hw *hw = &adapter->hw;
	u8 *addr = adapter->mac_table[index].addr;
	/* HW expects these in little endian so we reverse the byte order
	 * from network order (big endian) to little endian
	 */
	rar_low = ((u32) addr[0] | ((u32) addr[1] << 8) |
			   ((u32) addr[2] << 16) | ((u32) addr[3] << 24));
	rar_high = ((u32) addr[4] | ((u32) addr[5] << 8));
	
	/* Indicate to hardware the Address is Valid. */
	if (adapter->mac_table[index].state & IGB_MAC_STATE_IN_USE)
		rar_high |= E1000_RAH_AV;
	
	if (hw->mac.type == e1000_82575)
		rar_high |= E1000_RAH_POOL_1 * adapter->mac_table[index].queue;
	else
		rar_high |= E1000_RAH_POOL_1 << adapter->mac_table[index].queue;
	
	E1000_WRITE_REG(hw, E1000_RAL(index), rar_low);
	E1000_WRITE_FLUSH(hw);
	E1000_WRITE_REG(hw, E1000_RAH(index), rar_high);
	E1000_WRITE_FLUSH(hw);
}

/**
 * igb_sw_init - Initialize general software structures (struct igb_adapter)
 * @adapter: board private structure to initialize
 *
 * igb_sw_init initializes the Adapter private data structure.
 * Fields are initialized based on PCI device information and
 * OS network device settings (MTU size).
 **/
static int __devinit igb_sw_init(struct igb_adapter *adapter)
{
	struct e1000_hw *hw = &adapter->hw;
	IOEthernetController *netdev = adapter->netdev;
	IOPCIDevice *pdev = adapter->pdev;

	/* PCI config space info */

	hw->vendor_id = pdev->configRead16(kIOPCIConfigVendorID);
	hw->device_id = pdev->configRead16(kIOPCIConfigDeviceID);
	
	hw->subsystem_vendor_id = pdev->configRead16(kIOPCIConfigSubSystemVendorID);
	hw->subsystem_device_id = pdev->configRead16(kIOPCIConfigSubSystemID);

	hw->revision_id = pdev->configRead8(kIOPCIConfigRevisionID);

	hw->bus.pci_cmd_word = pdev->configRead16(kIOPCIConfigCommand);

	/* set default ring sizes */
	adapter->tx_ring_count = IGB_DEFAULT_TXD;
	adapter->rx_ring_count = IGB_DEFAULT_RXD;

	/* set default work limits */
	adapter->tx_work_limit = IGB_DEFAULT_TX_WORK;

	adapter->max_frame_size = ((AppleIGB*)netdev)->mtu() + ETH_HLEN + ETH_FCS_LEN +
					      VLAN_HLEN;

	/* Initialize the hardware-specific values */
	if (e1000_setup_init_funcs(hw, TRUE)) {
		pr_err( "Hardware Initialization Failure\n");
		return -EIO;
	}

	igb_check_options(adapter);

	adapter->mac_table = (igb_mac_addr*)kzalloc(sizeof(struct igb_mac_addr) * hw->mac.rar_entry_count);

	/* Setup and initialize a copy of the hw vlan table array */
	adapter->shadow_vfta = (u32 *)kzalloc(sizeof(u32) * E1000_VFTA_ENTRIES);

	/* These calls may decrease the number of queues */
 	if (hw->mac.type < e1000_i210)
		igb_set_sriov_capability(adapter);

	if (igb_init_interrupt_scheme(adapter, true)) {
		pr_err( "Unable to allocate memory for queues\n");
		return -ENOMEM;
	}

	/* Explicitly disable IRQ since the NIC can be in any state. */
	igb_irq_disable(adapter);

	set_bit(__IGB_DOWN, &adapter->state);
	return 0;
}


	
/**
 * igb_open - Called when a network interface is made active
 * @netdev: network interface device structure
 *
 * Returns 0 on success, negative value on failure
 *
 * The open entry point is called when a network interface is made
 * active by the system (IFF_UP).  At this point all resources needed
 * for transmit and receive operations are allocated, the interrupt
 * handler is registered with the OS, the watchdog timer is started,
 * and the stack is notified that the interface is ready.
 **/
int igb_open(IOEthernetController *netdev)
{
	struct igb_adapter *adapter = netdev_priv(netdev);
	struct e1000_hw *hw = &adapter->hw;
	int err;
	//int i;

    pr_debug("igb_open() ===>\n");

	/* disallow open during test */
    if (test_bit(__IGB_TESTING, &adapter->state)) {
        pr_err("disallowed open during test\n");
        return -EBUSY;
    }

	netif_carrier_off(netdev);
	
	/* allocate transmit descriptors */
	err = igb_setup_all_tx_resources(adapter);
    if (err) {
        pr_err("igb_setup_all_tx_resources failed\n");
        goto err_setup_tx;
    }

	/* allocate receive descriptors */
	err = igb_setup_all_rx_resources(adapter);
    if (err) {
        pr_err("igb_setup_all_rx_resources failed\n");
        goto err_setup_rx;
    }

	igb_power_up_link(adapter);
    pr_debug("Powered up link.\n");

	/* before we allocate an interrupt, we must be ready to handle it.
	 * Setting DEBUG_SHIRQ in the kernel makes it fire an interrupt
	 * as soon as we call pci_request_irq, so we have to setup our
	 * clean_rx handler before we do so.  */
	igb_configure(adapter);
	
	err = igb_request_irq(adapter);
    if (err) {
        pr_err("igb_request_irq failed\n");
        goto err_req_irq;
    }

#ifndef __APPLE__
	/* Notify the stack of the actual queue counts. */
	netif_set_real_num_tx_queues(netdev,
								 adapter->vmdq_pools ? 1 :
								 adapter->num_tx_queues);
	
	err = netif_set_real_num_rx_queues(netdev,
									   adapter->vmdq_pools ? 1 :
									   adapter->num_rx_queues);
	if (err)
		goto err_set_queues;
#endif

	/* From here on the code is the same as igb_up() */
	clear_bit(__IGB_DOWN, &adapter->state);

#ifndef __APPLE__
	for (i = 0; i < adapter->num_q_vectors; i++)
		napi_enable(&(adapter->q_vector[i]->napi));
#endif
	igb_configure_lli(adapter);
	
	/* Clear any pending interrupts. */
	E1000_READ_REG(hw, E1000_ICR);
	
	igb_irq_enable(adapter);
	
	/* notify VFs that reset has been completed */
	if (adapter->vfs_allocated_count) {
		u32 reg_data = E1000_READ_REG(hw, E1000_CTRL_EXT);
		reg_data |= E1000_CTRL_EXT_PFRSTD;
		E1000_WRITE_REG(hw, E1000_CTRL_EXT, reg_data);
	}

	netif_tx_start_all_queues(netdev);
	
	if (adapter->flags & IGB_FLAG_DETECT_BAD_DMA)
		schedule_work(&adapter->dma_err_task);
	
	/* start the watchdog. */
	hw->mac.get_link_status = 1;
    if (adapter->watchdog_task)
        schedule_work(&adapter->watchdog_task);
    else {
        pr_err("No watchdog_task set. Nothing scheduled. \n");
        err = 0xdeadbeef;
        goto err_req_irq;
    }

    pr_debug("igb_open() <===\n");
	return E1000_SUCCESS;

#ifndef __APPLE__
err_set_queues:
	igb_free_irq(adapter); // @todo Code will never be executed
#endif /*__APPLE__*/
err_req_irq:
	igb_release_hw_control(adapter);
	igb_power_down_link(adapter);
	igb_free_all_rx_resources(adapter);
err_setup_rx:
	igb_free_all_tx_resources(adapter);
err_setup_tx:
	igb_reset(adapter);
	
	return err;
}

/**
 * igb_close - Disables a network interface
 * @netdev: network interface device structure
 *
 * Returns 0, this is not allowed to fail
 *
 * The close entry point is called when an interface is de-activated
 * by the OS.  The hardware is still under the driver's control, but
 * needs to be disabled.  A global MAC reset is issued to stop the
 * hardware, and all transmit and receive resources are freed.
 **/
int igb_close(IOEthernetController *netdev)
{
	struct igb_adapter *adapter = netdev_priv(netdev);
	
	WARN_ON(test_bit(__IGB_RESETTING, &adapter->state));
	igb_down(adapter);
	
	/*CMW test */
	igb_release_hw_control(adapter);
	
	igb_free_irq(adapter);
	
	igb_free_all_tx_resources(adapter);
	igb_free_all_rx_resources(adapter);
	
	return 0;
}
	
/**
 * igb_setup_tx_resources - allocate Tx resources (Descriptors)
 * @tx_ring: tx descriptor ring (for a specific queue) to setup
 *
 * Return 0 on success, negative on failure
 **/
int igb_setup_tx_resources(struct igb_ring *tx_ring)
{
	//struct device *dev = tx_ring->dev;
	//int orig_node = dev_to_node(dev);
	int size;

	size = sizeof(struct igb_tx_buffer) * tx_ring->count;
	tx_ring->tx_buffer_info = (igb_tx_buffer*)vzalloc(size);
	if (!tx_ring->tx_buffer_info)
		goto err;

	/* round up to nearest 4K */
	tx_ring->size = tx_ring->count * sizeof(union e1000_adv_tx_desc);
	tx_ring->size = ALIGN(tx_ring->size, 4096);

	//set_dev_node(dev, orig_node);
#ifdef __APPLE__
	tx_ring->pool= IOBufferMemoryDescriptor::inTaskWithOptions( kernel_task,
							kIODirectionInOut | kIOMemoryPhysicallyContiguous,
							(vm_size_t)(tx_ring->size), PAGE_SIZE );
	
	if (!tx_ring->pool)
		goto err;
	tx_ring->pool->prepare();
	tx_ring->desc = tx_ring->pool->getBytesNoCopy();
	tx_ring->dma = tx_ring->pool->getPhysicalAddress();
#else
	tx_ring->desc = dma_alloc_coherent(dev,
						   tx_ring->size,
						   &tx_ring->dma,
						   GFP_KERNEL);
	
	if (!tx_ring->desc)
		goto err;
#endif

	tx_ring->next_to_use = 0;
	tx_ring->next_to_clean = 0;

	return 0;

err:
	vfree(tx_ring->tx_buffer_info, size);
	pr_err("Unable to allocate memory for the transmit descriptor ring\n");
	return -ENOMEM;
}

/**
 * igb_setup_all_tx_resources - wrapper to allocate Tx resources
 *				  (Descriptors) for all queues
 * @adapter: board private structure
 *
 * Return 0 on success, negative on failure
 **/
static int igb_setup_all_tx_resources(struct igb_adapter *adapter)
{
	//IOPCIDevice *pdev = adapter->pdev;
	int i, err = 0;

	for (i = 0; i < adapter->num_tx_queues; i++) {
		err = igb_setup_tx_resources(adapter->tx_ring[i]);
		if (err) {
			pr_err(
				"Allocation for Tx Queue %u failed\n", i);
			for (i--; i >= 0; i--)
				igb_free_tx_resources(adapter->tx_ring[i]);
			break;
		}
	}

	return err;
}

/**
 * igb_setup_tctl - configure the transmit control registers
 * @adapter: Board private structure
 **/
void igb_setup_tctl(struct igb_adapter *adapter)
{
	struct e1000_hw *hw = &adapter->hw;
	u32 tctl;

	/* disable queue 0 which is enabled by default on 82575 and 82576 */
	E1000_WRITE_REG(hw, E1000_TXDCTL(0), 0);

	/* Program the Transmit Control Register */
	tctl = E1000_READ_REG(hw, E1000_TCTL);
	tctl &= ~E1000_TCTL_CT;
	tctl |= E1000_TCTL_PSP | E1000_TCTL_RTLC |
		(E1000_COLLISION_THRESHOLD << E1000_CT_SHIFT);

	e1000_config_collision_dist(hw);

	/* Enable transmits */
	tctl |= E1000_TCTL_EN;

	E1000_WRITE_REG(hw, E1000_TCTL, tctl);
}

static u32 igb_tx_wthresh(struct igb_adapter *adapter)
{
	struct e1000_hw *hw = &adapter->hw;

	switch (hw->mac.type) {
		case e1000_i354:
			return 4;
		case e1000_82576:
			if (adapter->msix_entries)
				return 1;
		default:
			break;
	}
	
	return 16;
}

/**
 * igb_configure_tx_ring - Configure transmit ring after Reset
 * @adapter: board private structure
 * @ring: tx ring to configure
 *
 * Configure a transmit ring after a reset.
 **/
void igb_configure_tx_ring(struct igb_adapter *adapter,
                           struct igb_ring *ring)
{
	struct e1000_hw *hw = &adapter->hw;
	u32 txdctl = 0;
	u64 tdba = ring->dma;
	int reg_idx = ring->reg_idx;

	/* disable the queue */
	E1000_WRITE_REG(hw, E1000_TXDCTL(reg_idx), 0);
	E1000_WRITE_FLUSH(hw);
	mdelay(10);

	E1000_WRITE_REG(hw, E1000_TDLEN(reg_idx),
	                ring->count * sizeof(union e1000_adv_tx_desc));
	E1000_WRITE_REG(hw, E1000_TDBAL(reg_idx),
	                tdba & 0x00000000ffffffffULL);
	E1000_WRITE_REG(hw, E1000_TDBAH(reg_idx), tdba >> 32);

	ring->tail = adapter->io_addr + E1000_TDT(reg_idx);
	E1000_WRITE_REG(hw, E1000_TDH(reg_idx), 0);
	writel(0, ring->tail);

	txdctl |= IGB_TX_PTHRESH;
	txdctl |= IGB_TX_HTHRESH << 8;
	txdctl |= igb_tx_wthresh(adapter) << 16;

	txdctl |= E1000_TXDCTL_QUEUE_ENABLE;
	E1000_WRITE_REG(hw, E1000_TXDCTL(reg_idx), txdctl);
}

/**
 * igb_configure_tx - Configure transmit Unit after Reset
 * @adapter: board private structure
 *
 * Configure the Tx unit of the MAC after a reset.
 **/
static void igb_configure_tx(struct igb_adapter *adapter)
{
	int i;

	for (i = 0; i < adapter->num_tx_queues; i++)
		igb_configure_tx_ring(adapter, adapter->tx_ring[i]);
}

/**
 * igb_setup_rx_resources - allocate Rx resources (Descriptors)
 * @rx_ring:    rx descriptor ring (for a specific queue) to setup
 *
 * Returns 0 on success, negative on failure
 **/
int igb_setup_rx_resources(struct igb_ring *rx_ring)
{
	//struct device *dev = rx_ring->dev;
	//int orig_node = dev_to_node(dev);
	int size, desc_len;

	size = sizeof(struct igb_rx_buffer) * rx_ring->count;
	rx_ring->rx_buffer_info = (igb_rx_buffer*)vzalloc(size);
	if (!rx_ring->rx_buffer_info)
		goto err;

	desc_len = sizeof(union e1000_adv_rx_desc);

	/* Round up to nearest 4K */
	rx_ring->size = rx_ring->count * desc_len;
	rx_ring->size = ALIGN(rx_ring->size, 4096);
#ifdef __APPLE__
	rx_ring->pool= IOBufferMemoryDescriptor::inTaskWithOptions( kernel_task,
								kIODirectionInOut | kIOMemoryPhysicallyContiguous,
								(vm_size_t)(rx_ring->size), PAGE_SIZE );
	
	if (!rx_ring->pool)
		goto err;
	rx_ring->pool->prepare();
	rx_ring->desc = rx_ring->pool->getBytesNoCopy();
	rx_ring->dma = rx_ring->pool->getPhysicalAddress();
#else
	rx_ring->desc = dma_alloc_coherent(dev,
						   rx_ring->size,
						   &rx_ring->dma,
						   GFP_KERNEL);

	if (!rx_ring->desc)
		goto err;
#endif

	rx_ring->next_to_alloc = 0;
	rx_ring->next_to_clean = 0;
	rx_ring->next_to_use = 0;

	return 0;

err:
	vfree(rx_ring->rx_buffer_info,size);
	rx_ring->rx_buffer_info = NULL;
	pr_err("Unable to allocate memory for the receive descriptor ring\n");
	return -ENOMEM;
}

/**
 * igb_setup_all_rx_resources - wrapper to allocate Rx resources
 *				  (Descriptors) for all queues
 * @adapter: board private structure
 *
 * Return 0 on success, negative on failure
 **/
static int igb_setup_all_rx_resources(struct igb_adapter *adapter)
{
	//IOPCIDevice *pdev = adapter->pdev;
	int i, err = 0;

	for (i = 0; i < adapter->num_rx_queues; i++) {
		err = igb_setup_rx_resources(adapter->rx_ring[i]);
		if (err) {
			pr_err("Allocation for Rx Queue %u failed\n", i);
			for (i--; i >= 0; i--)
				igb_free_rx_resources(adapter->rx_ring[i]);
			break;
		}
	}

	return err;
}

/**
 * igb_setup_mrqc - configure the multiple receive queue control registers
 * @adapter: Board private structure
 **/
static void igb_setup_mrqc(struct igb_adapter *adapter)
{
	struct e1000_hw *hw = &adapter->hw;
	u32 mrqc, rxcsum;
	u32 j, num_rx_queues;
#ifndef ETHTOOL_SRXFHINDIR
	u32 shift = 0, shift2 = 0;
#endif /* ETHTOOL_SRXFHINDIR */
	static const u32 rsskey[10] = { 0xDA565A6D, 0xC20E5B25, 0x3D256741,
		0xB08FA343, 0xCB2BCAD0, 0xB4307BAE,
		0xA32DCB77, 0x0CF23080, 0x3BB7426A,
		0xFA01ACBE };
	
	/* Fill out hash function seeds */
	for (j = 0; j < 10; j++)
		E1000_WRITE_REG(hw, E1000_RSSRK(j), rsskey[j]);
	
	num_rx_queues = adapter->rss_queues;
	
#ifdef ETHTOOL_SRXFHINDIR
	if (hw->mac.type == e1000_82576) {
		/* 82576 supports 2 RSS queues for SR-IOV */
		if (adapter->vfs_allocated_count)
			num_rx_queues = 2;
	}
	if (adapter->rss_indir_tbl_init != num_rx_queues) {
		for (j = 0; j < IGB_RETA_SIZE; j++)
			adapter->rss_indir_tbl[j] = (j * num_rx_queues) / IGB_RETA_SIZE;
		adapter->rss_indir_tbl_init = num_rx_queues;
	}
	igb_write_rss_indir_tbl(adapter);
#else
	/* 82575 and 82576 supports 2 RSS queues for VMDq */
	switch (hw->mac.type) {
		case e1000_82575:
			if (adapter->vmdq_pools) {
				shift = 2;
				shift2 = 6;
			}
			shift = 6;
			break;
		case e1000_82576:
			/* 82576 supports 2 RSS queues for SR-IOV */
			if (adapter->vfs_allocated_count || adapter->vmdq_pools) {
				shift = 3;
				num_rx_queues = 2;
			}
			break;
		default:
			break;
	}
	
	/*
	 * Populate the redirection table 4 entries at a time.  To do this
	 * we are generating the results for n and n+2 and then interleaving
	 * those with the results with n+1 and n+3.
	 */
	for (j = 0; j < 32; j++) {
 		/* first pass generates n and n+2 */
 		u32 base = ((j * 0x00040004) + 0x00020000) * num_rx_queues;
 		u32 reta = (base & 0x07800780) >> (7 - shift);
		
		/* second pass generates n+1 and n+3 */
		base += 0x00010001 * num_rx_queues;
		reta |= (base & 0x07800780) << (1 + shift);
		
		/* generate 2nd table for 82575 based parts */
		if (shift2)
			reta |= (0x01010101 * num_rx_queues) << shift2;
		
		E1000_WRITE_REG(hw, E1000_RETA(j), reta);
	}
#endif /* ETHTOOL_SRXFHINDIR */

	/*
	 * Disable raw packet checksumming so that RSS hash is placed in
	 * descriptor on writeback.  No need to enable TCP/UDP/IP checksum
	 * offloads as they are enabled by default
	 */
	rxcsum = E1000_READ_REG(hw, E1000_RXCSUM);
	rxcsum |= E1000_RXCSUM_PCSD;
	
	if (adapter->hw.mac.type >= e1000_82576)
	/* Enable Receive Checksum Offload for SCTP */
		rxcsum |= E1000_RXCSUM_CRCOFL;
	
	/* Don't need to set TUOFL or IPOFL, they default to 1 */
	E1000_WRITE_REG(hw, E1000_RXCSUM, rxcsum);
	
	/* Generate RSS hash based on packet types, TCP/UDP
	 * port numbers and/or IPv4/v6 src and dst addresses
	 */
	mrqc = E1000_MRQC_RSS_FIELD_IPV4 |
	E1000_MRQC_RSS_FIELD_IPV4_TCP |
	E1000_MRQC_RSS_FIELD_IPV6 |
	E1000_MRQC_RSS_FIELD_IPV6_TCP |
	E1000_MRQC_RSS_FIELD_IPV6_TCP_EX;
	
	if (adapter->flags & IGB_FLAG_RSS_FIELD_IPV4_UDP)
		mrqc |= E1000_MRQC_RSS_FIELD_IPV4_UDP;
	if (adapter->flags & IGB_FLAG_RSS_FIELD_IPV6_UDP)
		mrqc |= E1000_MRQC_RSS_FIELD_IPV6_UDP;
	
	/* If VMDq is enabled then we set the appropriate mode for that, else
	 * we default to RSS so that an RSS hash is calculated per packet even
	 * if we are only using one queue */
	if (adapter->vfs_allocated_count || adapter->vmdq_pools) {
		if (hw->mac.type > e1000_82575) {
			/* Set the default pool for the PF's first queue */
			u32 vtctl = E1000_READ_REG(hw, E1000_VT_CTL);
			vtctl &= ~(E1000_VT_CTL_DEFAULT_POOL_MASK |
					   E1000_VT_CTL_DISABLE_DEF_POOL);
			vtctl |= adapter->vfs_allocated_count <<
			E1000_VT_CTL_DEFAULT_POOL_SHIFT;
			E1000_WRITE_REG(hw, E1000_VT_CTL, vtctl);
		}
		if (adapter->rss_queues > 1)
			mrqc |= E1000_MRQC_ENABLE_VMDQ_RSS_2Q;
		else
			mrqc |= E1000_MRQC_ENABLE_VMDQ;
	} else {
		mrqc |= E1000_MRQC_ENABLE_RSS_4Q;
	}
	igb_vmm_control(adapter);
	
	E1000_WRITE_REG(hw, E1000_MRQC, mrqc);
}

/**
 * igb_setup_rctl - configure the receive control registers
 * @adapter: Board private structure
 **/
void igb_setup_rctl(struct igb_adapter *adapter)
{
	struct e1000_hw *hw = &adapter->hw;
	u32 rctl;

	rctl = E1000_READ_REG(hw, E1000_RCTL);

	rctl &= ~(3 << E1000_RCTL_MO_SHIFT);
	rctl &= ~(E1000_RCTL_LBM_TCVR | E1000_RCTL_LBM_MAC);

	rctl |= E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_RDMTS_HALF |
		(hw->mac.mc_filter_type << E1000_RCTL_MO_SHIFT);

	/*
	 * enable stripping of CRC. It's unlikely this will break BMC
	 * redirection as it did with e1000. Newer features require
	 * that the HW strips the CRC.
	 */
	rctl |= E1000_RCTL_SECRC;

	/* disable store bad packets and clear size bits. */
	rctl &= ~(E1000_RCTL_SBP | E1000_RCTL_SZ_256);

	/* enable LPE to prevent packets larger than max_frame_size */
	rctl |= E1000_RCTL_LPE;

	/* disable queue 0 to prevent tail write w/o re-config */
	E1000_WRITE_REG(hw, E1000_RXDCTL(0), 0);

	/* Attention!!!  For SR-IOV PF driver operations you must enable
	 * queue drop for all VF and PF queues to prevent head of line blocking
	 * if an un-trusted VF does not provide descriptors to hardware.
	 */
	if (adapter->vfs_allocated_count) {
		/* set all queue drop enable bits */
		E1000_WRITE_REG(hw, E1000_QDE, ALL_QUEUES);
	}

	E1000_WRITE_REG(hw, E1000_RCTL, rctl);
}

static inline int igb_set_vf_rlpml(struct igb_adapter *adapter, int size,
                                   int vfn)
{
	struct e1000_hw *hw = &adapter->hw;
	u32 vmolr;

	/* if it isn't the PF check to see if VFs are enabled and
	 * increase the size to support vlan tags
	 */
	if (vfn < adapter->vfs_allocated_count &&
	    adapter->vf_data[vfn].vlans_enabled)
		size += VLAN_HLEN;

	vmolr = E1000_READ_REG(hw, E1000_VMOLR(vfn));
	vmolr &= ~E1000_VMOLR_RLPML_MASK;
	vmolr |= size | E1000_VMOLR_LPE;
	E1000_WRITE_REG(hw, E1000_VMOLR(vfn), vmolr);

	return 0;
}

/**
 * igb_rlpml_set - set maximum receive packet size
 * @adapter: board private structure
 *
 * Configure maximum receivable packet size.
 **/
static void igb_rlpml_set(struct igb_adapter *adapter)
{
	u32 max_frame_size = adapter->max_frame_size;
	struct e1000_hw *hw = &adapter->hw;
	u16 pf_id = adapter->vfs_allocated_count;

	if (adapter->vmdq_pools && hw->mac.type != e1000_82575) {
		int i;
		for (i = 0; i < adapter->vmdq_pools; i++)
			igb_set_vf_rlpml(adapter, max_frame_size, pf_id + i);
		/*
		 * If we're in VMDQ or SR-IOV mode, then set global RLPML
		 * to our max jumbo frame size, in case we need to enable
		 * jumbo frames on one of the rings later.
		 * This will not pass over-length frames into the default
		 * queue because it's gated by the VMOLR.RLPML.
		 */
		max_frame_size = MAX_JUMBO_FRAME_SIZE;
	}
	/* Set VF RLPML for the PF device. */
	if (adapter->vfs_allocated_count)
		igb_set_vf_rlpml(adapter, max_frame_size, pf_id);

	E1000_WRITE_REG(hw, E1000_RLPML, max_frame_size);
}

static inline void igb_set_vf_vlan_strip(struct igb_adapter *adapter,
					int vfn, bool enable)
{
	struct e1000_hw *hw = &adapter->hw;
	u32 val;
	void __iomem *reg;

	if (hw->mac.type < e1000_82576)
		return;

	if (hw->mac.type == e1000_i350)
		reg = hw->hw_addr + E1000_DVMOLR(vfn);
	else
		reg = hw->hw_addr + E1000_VMOLR(vfn);

	val = readl(reg);
	if (enable)
		val |= E1000_VMOLR_STRVLAN;
	else
		val &= ~(E1000_VMOLR_STRVLAN);
	writel(val, reg);
}
static inline void igb_set_vmolr(struct igb_adapter *adapter,
				 int vfn, bool aupe)
{
	struct e1000_hw *hw = &adapter->hw;
	u32 vmolr;

	/*
	 * This register exists only on 82576 and newer so if we are older then
	 * we should exit and do nothing
	 */
	if (hw->mac.type < e1000_82576)
		return;

	vmolr = E1000_READ_REG(hw, E1000_VMOLR(vfn));

	if (aupe)
		vmolr |= E1000_VMOLR_AUPE;        /* Accept untagged packets */
	else
		vmolr &= ~(E1000_VMOLR_AUPE); /* Tagged packets ONLY */

	/* clear all bits that might not be set */
	vmolr &= ~E1000_VMOLR_RSSE;

	if (adapter->rss_queues > 1 && vfn == adapter->vfs_allocated_count)
		vmolr |= E1000_VMOLR_RSSE; /* enable RSS */

	vmolr |= E1000_VMOLR_BAM;	   /* Accept broadcast */
	vmolr |= E1000_VMOLR_LPE;	   /* Accept long packets */

	E1000_WRITE_REG(hw, E1000_VMOLR(vfn), vmolr);
}

/**
 * igb_configure_rx_ring - Configure a receive ring after Reset
 * @adapter: board private structure
 * @ring: receive ring to be configured
 *
 * Configure the Rx unit of the MAC after a reset.
 **/
void igb_configure_rx_ring(struct igb_adapter *adapter,
                           struct igb_ring *ring)
{
	struct e1000_hw *hw = &adapter->hw;
	u64 rdba = ring->dma;
	int reg_idx = ring->reg_idx;
	u32 srrctl = 0, rxdctl = 0;

	/* disable the queue */
	E1000_WRITE_REG(hw, E1000_RXDCTL(reg_idx), 0);

	/* Set DMA base address registers */
	E1000_WRITE_REG(hw, E1000_RDBAL(reg_idx),
	                rdba & 0x00000000ffffffffULL);
	E1000_WRITE_REG(hw, E1000_RDBAH(reg_idx), rdba >> 32);
	E1000_WRITE_REG(hw, E1000_RDLEN(reg_idx),
	               ring->count * sizeof(union e1000_adv_rx_desc));

	/* initialize head and tail */
	ring->tail = adapter->io_addr + E1000_RDT(reg_idx);
	E1000_WRITE_REG(hw, E1000_RDH(reg_idx), 0);
	writel(0, ring->tail);

	/* reset next-to- use/clean to place SW in sync with hardwdare */
	ring->next_to_clean = 0;
	ring->next_to_use = 0;
#ifndef CONFIG_IGB_DISABLE_PACKET_SPLIT
	ring->next_to_alloc = 0;
	
#endif
	/* set descriptor configuration */
	srrctl = IGB_RX_HDR_LEN << E1000_SRRCTL_BSIZEHDRSIZE_SHIFT;
	srrctl |= IGB_RX_BUFSZ >> E1000_SRRCTL_BSIZEPKT_SHIFT;
	srrctl |= E1000_SRRCTL_DESCTYPE_ADV_ONEBUF;
#ifdef HAVE_PTP_1588_CLOCK
	if (hw->mac.type >= e1000_82580)
		srrctl |= E1000_SRRCTL_TIMESTAMP;
#endif /* HAVE_PTP_1588_CLOCK */
	/*
	 * We should set the drop enable bit if:
	 *  SR-IOV is enabled
	 *   or
	 *  Flow Control is disabled and number of RX queues > 1
	 *
	 *  This allows us to avoid head of line blocking for security
	 *  and performance reasons.
	 */
	if (adapter->vfs_allocated_count ||
	    (adapter->num_rx_queues > 1 &&
	     (hw->fc.requested_mode == e1000_fc_none ||
	      hw->fc.requested_mode == e1000_fc_rx_pause)))
		srrctl |= E1000_SRRCTL_DROP_EN;

	E1000_WRITE_REG(hw, E1000_SRRCTL(reg_idx), srrctl);

	/* set filtering for VMDQ pools */
	igb_set_vmolr(adapter, reg_idx & 0x7, true);

	rxdctl |= IGB_RX_PTHRESH;
	rxdctl |= IGB_RX_HTHRESH << 8;
	rxdctl |= IGB_RX_WTHRESH << 16;

	/* enable receive descriptor fetching */
	rxdctl |= E1000_RXDCTL_QUEUE_ENABLE;
	E1000_WRITE_REG(hw, E1000_RXDCTL(reg_idx), rxdctl);
}

/**
 * igb_configure_rx - Configure receive Unit after Reset
 * @adapter: board private structure
 *
 * Configure the Rx unit of the MAC after a reset.
 **/
static void igb_configure_rx(struct igb_adapter *adapter)
{
	int i;

	/* set UTA to appropriate mode */
	igb_set_uta(adapter);

	/* set the correct pool for the PF default MAC address in entry 0 */
	igb_rar_set_qsel(adapter, adapter->hw.mac.addr, 0,
					 adapter->vfs_allocated_count);
	
	/* Setup the HW Rx Head and Tail Descriptor Pointers and
	 * the Base and Length of the Rx Descriptor Ring
	 */
	for (i = 0; i < adapter->num_rx_queues; i++)
		igb_configure_rx_ring(adapter, adapter->rx_ring[i]);
}

/**
 * igb_free_tx_resources - Free Tx Resources per Queue
 * @tx_ring: Tx descriptor ring for a specific queue
 *
 * Free all transmit software resources
 **/
void igb_free_tx_resources(struct igb_ring *tx_ring)
{
	igb_clean_tx_ring(tx_ring);

	vfree(tx_ring->tx_buffer_info,sizeof(struct igb_tx_buffer) * tx_ring->count);
	tx_ring->tx_buffer_info = NULL;

	/* if not set, then don't free */
#ifdef __APPLE__
	if (!tx_ring->pool)
		return;
    
	tx_ring->pool->complete();
	tx_ring->pool->release();
	tx_ring->pool = NULL;
#else
	if (!tx_ring->desc)
		return;
    
	dma_free_coherent(tx_ring->dev, tx_ring->size,
			  tx_ring->desc, tx_ring->dma);
#endif

	tx_ring->desc = NULL;
}

/**
 * igb_free_all_tx_resources - Free Tx Resources for All Queues
 * @adapter: board private structure
 *
 * Free all transmit software resources
 **/
static void igb_free_all_tx_resources(struct igb_adapter *adapter)
{
	int i;

	for (i = 0; i < adapter->num_tx_queues; i++)
		igb_free_tx_resources(adapter->tx_ring[i]);
}

void igb_unmap_and_free_tx_resource(struct igb_ring *ring,
                                    struct igb_tx_buffer *tx_buffer)
{
    if (tx_buffer->skb) {
#ifdef __APPLE__
        ring->netdev->freePacket(tx_buffer->skb);
#else
        dev_kfree_skb_any(tx_buffer->skb);
        if (dma_unmap_len(tx_buffer, len))
            dma_unmap_single(ring->dev,
                             dma_unmap_addr(tx_buffer, dma),
                             dma_unmap_len(tx_buffer, len),
                             DMA_TO_DEVICE);
    } else if (dma_unmap_len(tx_buffer, len)) {
        dma_unmap_page(ring->dev,
                       dma_unmap_addr(tx_buffer, dma),
                       dma_unmap_len(tx_buffer, len),
                       DMA_TO_DEVICE);
#endif
    }
    tx_buffer->next_to_watch = NULL;
    tx_buffer->skb = NULL;
    dma_unmap_len_set(tx_buffer, len, 0);
    /* buffer_info must be completely set up in the transmit path */
}

/**
 * igb_clean_tx_ring - Free Tx Buffers
 * @tx_ring: ring to be cleaned
 **/
static void igb_clean_tx_ring(struct igb_ring *tx_ring)
{
	struct igb_tx_buffer *buffer_info;
	unsigned long size;
	u16 i;

	if (!tx_ring->tx_buffer_info)
		return;
	/* Free all the Tx ring sk_buffs */

	for (i = 0; i < tx_ring->count; i++) {
		buffer_info = &tx_ring->tx_buffer_info[i];
		igb_unmap_and_free_tx_resource(tx_ring, buffer_info);
	}

#ifndef __APPLE__
	netdev_tx_reset_queue(txring_txq(tx_ring));
#endif /* CONFIG_BQL */
	
	size = sizeof(struct igb_tx_buffer) * tx_ring->count;
	memset(tx_ring->tx_buffer_info, 0, size);

	/* Zero out the descriptor ring */
	memset(tx_ring->desc, 0, tx_ring->size);

	tx_ring->next_to_use = 0;
	tx_ring->next_to_clean = 0;
}

/**
 * igb_clean_all_tx_rings - Free Tx Buffers for all queues
 * @adapter: board private structure
 **/
static void igb_clean_all_tx_rings(struct igb_adapter *adapter)
{
	int i;

	for (i = 0; i < adapter->num_tx_queues; i++)
		igb_clean_tx_ring(adapter->tx_ring[i]);
}

/**
 * igb_free_rx_resources - Free Rx Resources
 * @rx_ring: ring to clean the resources from
 *
 * Free all receive software resources
 **/
void igb_free_rx_resources(struct igb_ring *rx_ring)
{
	igb_clean_rx_ring(rx_ring);

	vfree(rx_ring->rx_buffer_info,sizeof(struct igb_rx_buffer) * rx_ring->count);
	rx_ring->rx_buffer_info = NULL;

	/* if not set, then don't free */
	if (!rx_ring->desc)
		return;
#ifdef __APPLE__
	if(rx_ring->pool){
		rx_ring->pool->complete();
		rx_ring->pool->release();
		rx_ring->pool = NULL;
	}
#else
	dma_free_coherent(rx_ring->dev, rx_ring->size,
			  rx_ring->desc, rx_ring->dma);
#endif

	rx_ring->desc = NULL;
}

/**
 * igb_free_all_rx_resources - Free Rx Resources for All Queues
 * @adapter: board private structure
 *
 * Free all receive software resources
 **/
static void igb_free_all_rx_resources(struct igb_adapter *adapter)
{
	int i;

	for (i = 0; i < adapter->num_rx_queues; i++)
		igb_free_rx_resources(adapter->rx_ring[i]);
}

/**
 * igb_clean_rx_ring - Free Rx Buffers per Queue
 * @rx_ring: ring to free buffers from
 **/
void igb_clean_rx_ring(struct igb_ring *rx_ring)
{
	unsigned long size;
	u16 i;

	if (!rx_ring->rx_buffer_info)
		return;

	if (rx_ring->skb)
#ifdef	__APPLE__
        rx_ring->netdev->freePacket(rx_ring->skb);
#else
		dev_kfree_skb(rx_ring->skb);
#endif
	rx_ring->skb = NULL;
	
	/* Free all the Rx ring sk_buffs */
	for (i = 0; i < rx_ring->count; i++) {
		struct igb_rx_buffer *buffer_info = &rx_ring->rx_buffer_info[i];
		if (!buffer_info->page)
			continue;
        
#ifdef __APPLE__
        buffer_info->page->complete();
        buffer_info->page->release();
#else
		dma_unmap_page(rx_ring->dev,
                       buffer_info->dma,
                       PAGE_SIZE,
                       DMA_FROM_DEVICE);
		__free_page(buffer_info->page);
#endif	// __APPLE__
        
		buffer_info->page = NULL;
	}

	size = sizeof(struct igb_rx_buffer) * rx_ring->count;
	memset(rx_ring->rx_buffer_info, 0, size);

	/* Zero out the descriptor ring */
	memset(rx_ring->desc, 0, rx_ring->size);

	rx_ring->next_to_alloc = 0;
	rx_ring->next_to_clean = 0;
	rx_ring->next_to_use = 0;
}

/**
 * igb_clean_all_rx_rings - Free Rx Buffers for all queues
 * @adapter: board private structure
 **/
static void igb_clean_all_rx_rings(struct igb_adapter *adapter)
{
	int i;

	for (i = 0; i < adapter->num_rx_queues; i++)
		igb_clean_rx_ring(adapter->rx_ring[i]);
}

#ifndef __APPLE__
/**
 * igb_set_mac - Change the Ethernet Address of the NIC
 * @netdev: network interface device structure
 * @p: pointer to an address structure
 *
 * Returns 0 on success, negative on failure
 **/
static int igb_set_mac(IOEthernetController *netdev, void *p)
{
	struct igb_adapter *adapter = netdev_priv(netdev);
	struct e1000_hw *hw = &adapter->hw;
	struct sockaddr *addr = p;

	if (!is_valid_ether_addr(addr->sa_data))
		return -EADDRNOTAVAIL;

	memcpy(netdev->dev_addr, addr->sa_data, netdev->addr_len);
	memcpy(hw->mac.addr, addr->sa_data, netdev->addr_len);

	/* set the correct pool for the new PF MAC address in entry 0 */
	igb_rar_set_qsel(adapter, hw->mac.addr, 0,
					 adapter->vfs_allocated_count);
	
	return 0;
}
#endif

#ifndef __APPLE__
/**
 * igb_write_mc_addr_list - write multicast addresses to MTA
 * @netdev: network interface device structure
 *
 * Writes multicast address list to the MTA hash table.
 * Returns: -ENOMEM on failure
 *                0 on no addresses written
 *                X on writing X addresses to MTA
 **/
int igb_write_mc_addr_list(IOEthernetController *netdev)
{
	struct igb_adapter *adapter = netdev_priv(netdev);
	struct e1000_hw *hw = &adapter->hw;
#ifdef NETDEV_HW_ADDR_T_MULTICAST
	struct netdev_hw_addr *ha;
#else
	struct dev_mc_list *ha;
#endif
	u8  *mta_list;
	int i, count;
	count = netdev_mc_count(netdev);

	if (!count) {
		e1000_update_mc_addr_list(hw, NULL, 0);
		return 0;
	}
	mta_list = kzalloc(count * 6, GFP_ATOMIC);
	if (!mta_list)
		return -ENOMEM;

	/* The shared function expects a packed array of only addresses. */
	i = 0;
	netdev_for_each_mc_addr(ha, netdev)
#ifdef NETDEV_HW_ADDR_T_MULTICAST
		memcpy(mta_list + (i++ * ETH_ALEN), ha->addr, ETH_ALEN);
#else
		memcpy(mta_list + (i++ * ETH_ALEN), ha->dmi_addr, ETH_ALEN);
#endif
	e1000_update_mc_addr_list(hw, mta_list, i);
	kfree(mta_list);

	return count;
}
#endif

void igb_full_sync_mac_table(struct igb_adapter *adapter)
{
	struct e1000_hw *hw = &adapter->hw;
	int i;
	for (i = 0; i < hw->mac.rar_entry_count; i++){
			igb_rar_set(adapter, i);
	}
}

void igb_sync_mac_table(struct igb_adapter *adapter)
{
	struct e1000_hw *hw = &adapter->hw;
	int i;

	for (i = 0; i < hw->mac.rar_entry_count; i++) {
		if (adapter->mac_table[i].state & IGB_MAC_STATE_MODIFIED)
			igb_rar_set(adapter, i);
		adapter->mac_table[i].state &= ~(IGB_MAC_STATE_MODIFIED);
	}
}

int igb_available_rars(struct igb_adapter *adapter)
{
	struct e1000_hw *hw = &adapter->hw;
	int i, count = 0;

	for (i = 0; i < hw->mac.rar_entry_count; i++) {
		if (adapter->mac_table[i].state == 0)
			count++;
	}
	return count;
}

static void igb_rar_set_qsel(struct igb_adapter *adapter, u8 *addr, u32 index,
							 u8 qsel)
{
	u32 rar_low, rar_high;
	struct e1000_hw *hw = &adapter->hw;
	
	/* HW expects these in little endian so we reverse the byte order
	 * from network order (big endian) to little endian
	 */
	rar_low = ((u32) addr[0] | ((u32) addr[1] << 8) |
			   ((u32) addr[2] << 16) | ((u32) addr[3] << 24));
	rar_high = ((u32) addr[4] | ((u32) addr[5] << 8));
	
	/* Indicate to hardware the Address is Valid. */
	rar_high |= E1000_RAH_AV;
	
	if (hw->mac.type == e1000_82575)
		rar_high |= E1000_RAH_POOL_1 * qsel;
	else
		rar_high |= E1000_RAH_POOL_1 << qsel;
	
	E1000_WRITE_REG(hw, E1000_RAL(index), rar_low);
	E1000_WRITE_FLUSH(hw);
	E1000_WRITE_REG(hw, E1000_RAH(index), rar_high);
	E1000_WRITE_FLUSH(hw);
}
	
#ifdef HAVE_SET_RX_MODE
/**
 * igb_write_uc_addr_list - write unicast addresses to RAR table
 * @netdev: network interface device structure
 *
 * Writes unicast address list to the RAR table.
 * Returns: -ENOMEM on failure/insufficient address space
 *                0 on no addresses written
 *                X on writing X addresses to the RAR table
 **/
static int igb_write_uc_addr_list(IOEthernetController *netdev)
{
	struct igb_adapter *adapter = netdev_priv(netdev);
	struct e1000_hw *hw = &adapter->hw;
	unsigned int vfn = adapter->vfs_allocated_count;
	unsigned int rar_entries = hw->mac.rar_entry_count - (vfn + 1);
	int count = 0;

	/* return ENOMEM indicating insufficient memory for addresses */
	if (netdev_uc_count(netdev) > igb_available_rars(adapter))
		return -ENOMEM;
	if (!netdev_uc_empty(netdev) && rar_entries) {
#ifdef NETDEV_HW_ADDR_T_UNICAST
		struct netdev_hw_addr *ha;
#else
		struct dev_mc_list *ha;
#endif
		netdev_for_each_uc_addr(ha, netdev) {
#ifdef NETDEV_HW_ADDR_T_UNICAST
			if (!rar_entries)
				break;
			igb_rar_set_qsel(adapter, ha->addr,
							 rar_entries--,
							 vfn);
#else
			igb_rar_set_qsel(adapter, ha->da_addr,
							 rar_entries--,
							 vfn);
#endif
			count++;
		}
	}
	
	/* write the addresses in reverse order to avoid write combining */
	for (; rar_entries > 0 ; rar_entries--) {
		E1000_WRITE_REG(hw, E1000_RAH(rar_entries), 0);
		E1000_WRITE_REG(hw, E1000_RAL(rar_entries), 0);
	}
	E1000_WRITE_FLUSH(hw);
	return count;
}

#endif /* HAVE_SET_RX_MODE */
/**
 * igb_set_rx_mode - Secondary Unicast, Multicast and Promiscuous mode set
 * @netdev: network interface device structure
 *
 * The set_rx_mode entry point is called whenever the unicast or multicast
 * address lists or the network interface flags are updated.  This routine is
 * responsible for configuring the hardware for proper unicast, multicast,
 * promiscuous mode, and all-multi behavior.
 **/
static void igb_set_rx_mode(IOEthernetController *netdev)
{
	struct igb_adapter *adapter = netdev_priv(netdev);
	struct e1000_hw *hw = &adapter->hw;
	unsigned int vfn = adapter->vfs_allocated_count;
	u32 rctl, vmolr = 0;
	//int count;

	/* Check for Promiscuous and All Multicast modes */
	rctl = E1000_READ_REG(hw, E1000_RCTL);

	/* clear the effected bits */
	rctl &= ~(E1000_RCTL_UPE | E1000_RCTL_MPE | E1000_RCTL_VFE);

	if (((AppleIGB*)netdev)->flags() & IFF_PROMISC) {
		vmolr |= (E1000_VMOLR_ROPE | E1000_VMOLR_MPME);
		/* retain VLAN HW filtering if in VT mode */
		if (adapter->vfs_allocated_count || adapter->vmdq_pools)
			rctl |= E1000_RCTL_VFE;
	} else {
		if (((AppleIGB*)netdev)->flags() & IFF_ALLMULTI) {
			rctl |= E1000_RCTL_MPE;
			vmolr |= E1000_VMOLR_MPME;
		} else {
			/*
			 * Write addresses to the MTA, if the attempt fails
			 * then we should just turn on promiscuous mode so
			 * that we can at least receive multicast traffic
			 */
			vmolr |= E1000_VMOLR_ROMPE;
		}
#ifdef HAVE_SET_RX_MODE
		/*
		 * Write addresses to available RAR registers, if there is not
		 * sufficient space to store all the addresses then enable
		 * unicast promiscuous mode
		 */
		count = igb_write_uc_addr_list(netdev);
		if (count < 0) {
			rctl |= E1000_RCTL_UPE;
			vmolr |= E1000_VMOLR_ROPE;
		}
#endif /* HAVE_SET_RX_MODE */
		rctl |= E1000_RCTL_VFE;
	}
	E1000_WRITE_REG(hw, E1000_RCTL, rctl);

	/*
	 * In order to support SR-IOV and eventually VMDq it is necessary to set
	 * the VMOLR to enable the appropriate modes.  Without this workaround
	 * we will have issues with VLAN tag stripping not being done for frames
	 * that are only arriving because we are the default pool
	 */
	if (hw->mac.type < e1000_82576)
		return;

	vmolr |= E1000_READ_REG(hw, E1000_VMOLR(vfn)) &
	         ~(E1000_VMOLR_ROPE | E1000_VMOLR_MPME | E1000_VMOLR_ROMPE);
	E1000_WRITE_REG(hw, E1000_VMOLR(vfn), vmolr);
	igb_restore_vf_multicasts(adapter);
}

static void igb_check_wvbr(struct igb_adapter *adapter)
{
	struct e1000_hw *hw = &adapter->hw;
	u32 wvbr = 0;

	switch (hw->mac.type) {
	case e1000_82576:
	case e1000_i350:
		wvbr = E1000_READ_REG(hw, E1000_WVBR);
		if (!wvbr)
			return;
		break;
	default:
		break;
	}

	adapter->wvbr |= wvbr;
}

#define IGB_STAGGERED_QUEUE_OFFSET 8

static void igb_spoof_check(struct igb_adapter *adapter)
{
	int j;

	if (!adapter->wvbr)
		return;

	switch (adapter->hw.mac.type) {
		case e1000_82576:
			for (j = 0; j < adapter->vfs_allocated_count; j++) {
				if (adapter->wvbr & (1 << j) ||
					adapter->wvbr & (1 << (j + IGB_STAGGERED_QUEUE_OFFSET))) {
					DPRINTK(DRV, WARNING,
							"Spoof event(s) detected on VF %d\n", j);
					adapter->wvbr &=
					~((1 << j) |
					  (1 << (j + IGB_STAGGERED_QUEUE_OFFSET)));
				}
			}
			break;
		case e1000_i350:
			for (j = 0; j < adapter->vfs_allocated_count; j++) {
				if (adapter->wvbr & (1 << j)) {
					DPRINTK(DRV, WARNING,
							"Spoof event(s) detected on VF %d\n", j);
					adapter->wvbr &= ~(1 << j);
				}
			}
			break;
		default:
			break;
	}
}

/* Need to wait a few seconds after link up to get diagnostic information from
 * the phy
 */
static void igb_update_phy_info(unsigned long data)
{
	struct igb_adapter *adapter = (struct igb_adapter *) data;

	e1000_get_phy_info(&adapter->hw);
}

/**
 * igb_has_link - check shared code for link and determine up/down
 * @adapter: pointer to driver private info
 **/
bool igb_has_link(struct igb_adapter *adapter)
{
	struct e1000_hw *hw = &adapter->hw;
	bool link_active = FALSE;

	/* get_link_status is set on LSC (link status) interrupt or
	 * rx sequence error interrupt.  get_link_status will stay
	 * false until the e1000_check_for_link establishes link
	 * for copper adapters ONLY
	 */
	switch (hw->phy.media_type) {
	case e1000_media_type_copper:
		if (!hw->mac.get_link_status)
			return true;
	case e1000_media_type_internal_serdes:
		e1000_check_for_link(hw);
		link_active = !hw->mac.get_link_status;
		break;
	case e1000_media_type_unknown:
	default:
		break;
	}

	if (((hw->mac.type == e1000_i210) ||
	     (hw->mac.type == e1000_i211)) &&
		(hw->phy.id == I210_I_PHY_ID)) {
		if (!netif_carrier_ok(adapter->netdev)) {
			adapter->flags &= ~IGB_FLAG_NEED_LINK_UPDATE;
		} else if (!(adapter->flags & IGB_FLAG_NEED_LINK_UPDATE)) {
			adapter->flags |= IGB_FLAG_NEED_LINK_UPDATE;
			adapter->link_check_timeout = jiffies;
		}
	}

	return link_active;
}

static void igb_dma_err_task(struct igb_adapter *adapter,IOTimerEventSource * src)
{
		int vf;
		struct e1000_hw *hw = &adapter->hw;
		u32 hgptc;
		u32 ciaa, ciad;
		
		hgptc = E1000_READ_REG(hw, E1000_HGPTC);
		if (hgptc) /* If incrementing then no need for the check below */
			goto dma_timer_reset;
		/*
		 * Check to see if a bad DMA write target from an errant or
		 * malicious VF has caused a PCIe error.  If so then we can
		 * issue a VFLR to the offending VF(s) and then resume without
		 * requesting a full slot reset.
		 */
		
		for (vf = 0; vf < adapter->vfs_allocated_count; vf++) {
			ciaa = (vf << 16) | 0x80000000;
			/* 32 bit read so align, we really want status at offset 6 */
			ciaa |= PCI_COMMAND;
			E1000_WRITE_REG(hw, E1000_CIAA, ciaa);
			ciad = E1000_READ_REG(hw, E1000_CIAD);
			ciaa &= 0x7FFFFFFF;
			/* disable debug mode asap after reading data */
			E1000_WRITE_REG(hw, E1000_CIAA, ciaa);
			/* Get the upper 16 bits which will be the PCI status reg */
			ciad >>= 16;
			if (ciad & (PCI_STATUS_REC_MASTER_ABORT |
						PCI_STATUS_REC_TARGET_ABORT |
						PCI_STATUS_SIG_SYSTEM_ERROR)) {
				pr_err("VF %d suffered error\n", vf);
				/* Issue VFLR */
				ciaa = (vf << 16) | 0x80000000;
				ciaa |= 0xA8;
				E1000_WRITE_REG(hw, E1000_CIAA, ciaa);
				ciad = 0x00008000;  /* VFLR */
				E1000_WRITE_REG(hw, E1000_CIAD, ciad);
				ciaa &= 0x7FFFFFFF;
				E1000_WRITE_REG(hw, E1000_CIAA, ciaa);
			}
		}
dma_timer_reset:
	/* Reset the timer */
	if (!test_bit(__IGB_DOWN, &adapter->state))
		src->setTimeoutMS(100);
}
	
	
	
enum latency_range {
	lowest_latency = 0,
	low_latency = 1,
	bulk_latency = 2,
	latency_invalid = 255
};

/**
 * igb_update_ring_itr - update the dynamic ITR value based on packet size
 *
 *      Stores a new ITR value based on strictly on packet size.  This
 *      algorithm is less sophisticated than that used in igb_update_itr,
 *      due to the difficulty of synchronizing statistics across multiple
 *      receive rings.  The divisors and thresholds used by this function
 *      were determined based on theoretical maximum wire speed and testing
 *      data, in order to minimize response time while increasing bulk
 *      throughput.
 *      This functionality is controlled by the InterruptThrottleRate module
 *      parameter (see igb_param.c)
 *      NOTE:  This function is called only when operating in a multiqueue
 *             receive environment.
 * @q_vector: pointer to q_vector
 **/
static void igb_update_ring_itr(struct igb_q_vector *q_vector)
{
	int new_val = q_vector->itr_val;
	int avg_wire_size = 0;
	struct igb_adapter *adapter = q_vector->adapter;
	unsigned int packets;

	/* For non-gigabit speeds, just fix the interrupt rate at 4000
	 * ints/sec - ITR timer value of 120 ticks.
	 */
	switch (adapter->link_speed) {
		case SPEED_10:
		case SPEED_100:
			new_val = IGB_4K_ITR;
			goto set_itr_val;
		default:
			break;
	}

	packets = q_vector->rx.total_packets;
	if (packets)
		avg_wire_size = q_vector->rx.total_bytes / packets;

	packets = q_vector->tx.total_packets;
	if (packets)
		avg_wire_size = max_t(u32, avg_wire_size,
		                      q_vector->tx.total_bytes / packets);

	/* if avg_wire_size isn't set no work was done */
	if (!avg_wire_size)
		goto clear_counts;

	/* Add 24 bytes to size to account for CRC, preamble, and gap */
	avg_wire_size += 24;

	/* Don't starve jumbo frames */
	avg_wire_size = min(avg_wire_size, 3000);

	/* Give a little boost to mid-size frames */
	if ((avg_wire_size > 300) && (avg_wire_size < 1200))
		new_val = avg_wire_size / 3;
	else
		new_val = avg_wire_size / 2;

	/* conservative mode (itr 3) eliminates the lowest_latency setting */
	if (new_val < IGB_20K_ITR &&
	    ((q_vector->rx.ring && adapter->rx_itr_setting == 3) ||
	     (!q_vector->rx.ring && adapter->tx_itr_setting == 3)))
		new_val = IGB_20K_ITR;

set_itr_val:
	if (new_val != q_vector->itr_val) {
		q_vector->itr_val = new_val;
		q_vector->set_itr = 1;
	}
clear_counts:
	q_vector->rx.total_bytes = 0;
	q_vector->rx.total_packets = 0;
	q_vector->tx.total_bytes = 0;
	q_vector->tx.total_packets = 0;
}

/**
 * igb_update_itr - update the dynamic ITR value based on statistics
 *      Stores a new ITR value based on packets and byte
 *      counts during the last interrupt.  The advantage of per interrupt
 *      computation is faster updates and more accurate ITR for the current
 *      traffic pattern.  Constants in this function were computed
 *      based on theoretical maximum wire speed and thresholds were set based
 *      on testing data as well as attempting to minimize response time
 *      while increasing bulk throughput.
 *      this functionality is controlled by the InterruptThrottleRate module
 *      parameter (see igb_param.c)
 *      NOTE:  These calculations are only valid when operating in a single-
 *             queue environment.
 * @q_vector: pointer to q_vector
 * @ring_container: ring info to update the itr for
 **/
static void igb_update_itr(struct igb_q_vector *q_vector,
			   struct igb_ring_container *ring_container)
{
	unsigned int packets = ring_container->total_packets;
	unsigned int bytes = ring_container->total_bytes;
	u8 itrval = ring_container->itr;

	/* no packets, exit with status unchanged */
	if (packets == 0)
		return;

	switch (itrval) {
	case lowest_latency:
		/* handle TSO and jumbo frames */
		if (bytes/packets > 8000)
			itrval = bulk_latency;
		else if ((packets < 5) && (bytes > 512))
			itrval = low_latency;
		break;
	case low_latency:  /* 50 usec aka 20000 ints/s */
		if (bytes > 10000) {
			/* this if handles the TSO accounting */
			if (bytes/packets > 8000) {
				itrval = bulk_latency;
			} else if ((packets < 10) || ((bytes/packets) > 1200)) {
				itrval = bulk_latency;
			} else if ((packets > 35)) {
				itrval = lowest_latency;
			}
		} else if (bytes/packets > 2000) {
			itrval = bulk_latency;
		} else if (packets <= 2 && bytes < 512) {
			itrval = lowest_latency;
		}
		break;
	case bulk_latency: /* 250 usec aka 4000 ints/s */
		if (bytes > 25000) {
			if (packets > 35)
				itrval = low_latency;
		} else if (bytes < 1500) {
			itrval = low_latency;
		}
		break;
	}

	/* clear work counters since we have the values we need */
	ring_container->total_bytes = 0;
	ring_container->total_packets = 0;

	/* write updated itr to ring container */
	ring_container->itr = itrval;
}

static void igb_set_itr(struct igb_q_vector *q_vector)
{
	struct igb_adapter *adapter = q_vector->adapter;
	u32 new_itr = q_vector->itr_val;
	u8 current_itr = 0;

	/* for non-gigabit speeds, just fix the interrupt rate at 4000 */
	switch (adapter->link_speed) {
		case SPEED_10:
		case SPEED_100:
			current_itr = 0;
			new_itr = IGB_4K_ITR;
			goto set_itr_now;
		default:
			break;
	}

	igb_update_itr(q_vector, &q_vector->tx);
	igb_update_itr(q_vector, &q_vector->rx);

	current_itr = max(q_vector->rx.itr, q_vector->tx.itr);

	/* conservative mode (itr 3) eliminates the lowest_latency setting */
	if (current_itr == lowest_latency &&
	    ((q_vector->rx.ring && adapter->rx_itr_setting == 3) ||
	     (!q_vector->rx.ring && adapter->tx_itr_setting == 3)))
		current_itr = low_latency;

	switch (current_itr) {
	/* counts and packets in update_itr are dependent on these numbers */
	case lowest_latency:
		new_itr = IGB_70K_ITR; /* 70,000 ints/sec */
		break;
	case low_latency:
		new_itr = IGB_20K_ITR; /* 20,000 ints/sec */
		break;
	case bulk_latency:
		new_itr = IGB_4K_ITR;  /* 4,000 ints/sec */
		break;
	default:
		break;
	}

set_itr_now:
	if (new_itr != q_vector->itr_val) {
		/* this attempts to bias the interrupt rate towards Bulk
		 * by adding intermediate steps when interrupt rate is
		 * increasing */
		new_itr = new_itr > q_vector->itr_val ?
		             max((new_itr * q_vector->itr_val) /
		                 (new_itr + (q_vector->itr_val >> 2)),
				 new_itr) :
			     new_itr;
		/* Don't write the value here; it resets the adapter's
		 * internal timer, and causes us to delay far longer than
		 * we should between interrupts.  Instead, we write the ITR
		 * value at the beginning of the next interrupt so the timing
		 * ends up being correct.
		 */
		q_vector->itr_val = new_itr;
		q_vector->set_itr = 1;
	}
}

void igb_tx_ctxtdesc(struct igb_ring *tx_ring, u32 vlan_macip_lens,
		     u32 type_tucmd, u32 mss_l4len_idx)
{
	struct e1000_adv_tx_context_desc *context_desc;
	u16 i = tx_ring->next_to_use;

	context_desc = IGB_TX_CTXTDESC(tx_ring, i);

	i++;
	tx_ring->next_to_use = (i < tx_ring->count) ? i : 0;

	/* set bits to identify this as an advanced context descriptor */
	type_tucmd |= E1000_TXD_CMD_DEXT | E1000_ADVTXD_DTYP_CTXT;

	/* For 82575, context index must be unique per ring. */
	if (test_bit(IGB_RING_FLAG_TX_CTX_IDX, &tx_ring->flags))
		mss_l4len_idx |= tx_ring->reg_idx << 4;

	context_desc->vlan_macip_lens	= cpu_to_le32(vlan_macip_lens);
	context_desc->seqnum_seed	= 0;
	context_desc->type_tucmd_mlhl	= cpu_to_le32(type_tucmd);
	context_desc->mss_l4len_idx	= cpu_to_le32(mss_l4len_idx);
}

#ifdef __APPLE__
// copy from bsd/netinet/in_cksum.c
union s_util {
	char    c[2];
	u_short s;
};

union l_util {
	u_int16_t s[2];
	u_int32_t l;
};

union q_util {
	u_int16_t s[4];
	u_int32_t l[2];
	u_int64_t q;
};
	
#define ADDCARRY(x)  (x > 65535 ? x -= 65535 : x)
#define REDUCE16													\
{																	\
q_util.q = sum;														\
l_util.l = q_util.s[0] + q_util.s[1] + q_util.s[2] + q_util.s[3];	\
sum = l_util.s[0] + l_util.s[1];									\
ADDCARRY(sum);														\
}
	
static inline u_short
in_pseudo(u_int a, u_int b, u_int c)
{
	u_int64_t sum;
	union q_util q_util;
	union l_util l_util;
	
	sum = (u_int64_t) a + b + c;
	REDUCE16;
	return (sum);
}

// copy from bsd/netinet6/in6_cksum.c
	
static inline u_short
in_pseudo6(struct ip6_hdr *ip6, int nxt, u_int32_t len)
{
	u_int16_t *w;
	int sum = 0;
	union {
		u_int16_t phs[4];
		struct {
			u_int32_t	ph_len;
			u_int8_t	ph_zero[3];
			u_int8_t	ph_nxt;
		} ph __attribute__((__packed__));
	} uph;
	
	bzero(&uph, sizeof (uph));
	
	/*
	 * First create IP6 pseudo header and calculate a summary.
	 */
	w = (u_int16_t *)&ip6->ip6_src;
	uph.ph.ph_len = htonl(len);
	uph.ph.ph_nxt = nxt;
	
	/* IPv6 source address */
	sum += w[0];
	if (!IN6_IS_SCOPE_LINKLOCAL(&ip6->ip6_src))
		sum += w[1];
	sum += w[2]; sum += w[3]; sum += w[4]; sum += w[5];
	sum += w[6]; sum += w[7];
	/* IPv6 destination address */
	sum += w[8];
	if (!IN6_IS_SCOPE_LINKLOCAL(&ip6->ip6_dst))
		sum += w[9];
	sum += w[10]; sum += w[11]; sum += w[12]; sum += w[13];
	sum += w[14]; sum += w[15];
	/* Payload length and upper layer identifier */
	sum += uph.phs[0];  sum += uph.phs[1];
	sum += uph.phs[2];  sum += uph.phs[3];
	
	return (u_short)sum;
}
#endif

static int igb_tso(struct igb_ring *tx_ring,
		   struct igb_tx_buffer *first,
		   u8 *hdr_len)
{
#ifdef NETIF_F_TSO
	struct sk_buff *skb = first->skb;
	u32 vlan_macip_lens, type_tucmd;
	u32 mss_l4len_idx, l4len;
	
#ifdef	__APPLE__
	mbuf_tso_request_flags_t request;
	u_int32_t mssValue;
	if(mbuf_get_tso_requested(skb, &request, &mssValue) || request == 0 )
		return 0;
#else
	if (skb->ip_summed != CHECKSUM_PARTIAL)
		return 0;
	
	if (!skb_is_gso(skb))
		return 0;

	if (skb_header_cloned(skb)) {
		int err = pskb_expand_head(skb, 0, 0, GFP_ATOMIC);
		if (err)
			return err;
	}
#endif

	/* ADV DTYP TUCMD MKRLOC/ISCSIHEDLEN */
	type_tucmd = E1000_ADVTXD_TUCMD_L4T_TCP;

#ifdef	__APPLE__
	u_int32_t dataLen = mbuf_pkthdr_len(skb);
	struct tcphdr* tcph;
	int ip_len;
	if (request & MBUF_TSO_IPV4) {
		struct ip *iph = ip_hdr(skb);
		tcph = tcp_hdr(skb);
		ip_len = ((u8*)tcph - (u8*)iph);
		iph->ip_len = 0;
		iph->ip_sum = 0;
		tcph->th_sum = in_pseudo(iph->ip_src.s_addr, iph->ip_dst.s_addr,
						 htonl(IPPROTO_TCP));
		type_tucmd |= E1000_ADVTXD_TUCMD_IPV4;
		first->tx_flags |= IGB_TX_FLAGS_TSO |
		IGB_TX_FLAGS_CSUM |
		IGB_TX_FLAGS_IPV4;
	} else {
		struct ip6_hdr *iph = ip6_hdr(skb);
		tcph = tcp6_hdr(skb);
		ip_len = ((u8*)tcph - (u8*)iph);
		iph->ip6_ctlun.ip6_un1.ip6_un1_plen = 0;
		tcph->th_sum = in_pseudo6(iph, IPPROTO_TCP, 0);
		first->tx_flags |= IGB_TX_FLAGS_TSO |
		IGB_TX_FLAGS_CSUM;
	}

	/* compute header lengths */
	l4len = tcph->th_off << 2;
	*hdr_len = ETHER_HDR_LEN + ip_len + l4len;

	u16 gso_segs = ((dataLen - *hdr_len) + (mssValue-1))/mssValue;

	/* update gso size and bytecount with header size */
	first->gso_segs = gso_segs;
	first->bytecount += (first->gso_segs - 1) * *hdr_len;
	
	/* MSS L4LEN IDX */
	mss_l4len_idx = l4len << E1000_ADVTXD_L4LEN_SHIFT;
	mss_l4len_idx |= mssValue << E1000_ADVTXD_MSS_SHIFT;
	
	/* VLAN MACLEN IPLEN */
	vlan_macip_lens = ip_len;
	vlan_macip_lens |= ETHER_HDR_LEN << E1000_ADVTXD_MACLEN_SHIFT;
	vlan_macip_lens |= first->tx_flags & IGB_TX_FLAGS_VLAN_MASK;
#else /* __APPLE__ */
	if (first->protocol == htons(ETH_P_IP)) {
		struct iphdr *iph = ip_hdr(skb);
		iph->tot_len = 0;
		iph->check = 0;
		tcp_hdr(skb)->check = ~csum_tcpudp_magic(iph->saddr,
												  iph->daddr, 0,
												  IPPROTO_TCP,
												  0);
		type_tucmd |= E1000_ADVTXD_TUCMD_IPV4;
		first->tx_flags |= IGB_TX_FLAGS_TSO |
		IGB_TX_FLAGS_CSUM |
		IGB_TX_FLAGS_IPV4;
#ifdef NETIF_F_TSO6
	} else if (skb_is_gso_v6(skb)) {
		ipv6_hdr(skb)->payload_len = 0;
		tcp_hdr(skb)->check = ~csum_ipv6_magic(&ipv6_hdr(skb)->saddr,
											   &ipv6_hdr(skb)->daddr,
											   0, IPPROTO_TCP, 0);
		first->tx_flags |= IGB_TX_FLAGS_TSO |
		IGB_TX_FLAGS_CSUM;
#endif
	}
	
	/* compute header lengths */
	l4len = tcp_hdrlen(skb);
	*hdr_len = skb_transport_offset(skb) + l4len;
	
	/* update gso size and bytecount with header size */
	first->gso_segs = skb_shinfo(skb)->gso_segs;
	first->bytecount += (first->gso_segs - 1) * *hdr_len;
	
	/* MSS L4LEN IDX */
	mss_l4len_idx = l4len << E1000_ADVTXD_L4LEN_SHIFT;
	mss_l4len_idx |= skb_shinfo(skb)->gso_size << E1000_ADVTXD_MSS_SHIFT;
	
	/* VLAN MACLEN IPLEN */
	vlan_macip_lens = skb_network_header_len(skb);
	vlan_macip_lens |= skb_network_offset(skb) << E1000_ADVTXD_MACLEN_SHIFT;
	vlan_macip_lens |= first->tx_flags & IGB_TX_FLAGS_VLAN_MASK;
#endif /* __APPLE__ */
	
	igb_tx_ctxtdesc(tx_ring, vlan_macip_lens, type_tucmd, mss_l4len_idx);
	
	return 1;
#else
	return 0;
#endif  /* NETIF_F_TSO */
}

// copy for accessing c++ constants

static void igb_tx_csum(struct igb_ring *tx_ring, struct igb_tx_buffer *first)
{
	struct sk_buff *skb = first->skb;
	u32 vlan_macip_lens = 0;
	u32 mss_l4len_idx = 0;
	u32 type_tucmd = 0;

#ifdef __APPLE__
#define     DEMAND_IPv6 (CSUM_TCPIPv6|CSUM_UDPIPv6)
#define     DEMAND_IPv4 (IONetworkController::kChecksumIP|IONetworkController::kChecksumTCP|IONetworkController::kChecksumUDP)
#define     DEMAND_MASK (DEMAND_IPv6|DEMAND_IPv4)

	UInt32 checksumDemanded;
	tx_ring->netdev->getChecksumDemand(skb, IONetworkController::kChecksumFamilyTCPIP, &checksumDemanded);
	checksumDemanded &= DEMAND_MASK;

	int  ehdrlen = ETHER_HDR_LEN;
	if(checksumDemanded == 0){
		if (!(first->tx_flags & IGB_TX_FLAGS_VLAN))
			return;
	} else {
		int  ip_hlen;
		u8* packet;
		
		/* Set the ether header length */
		packet = (u8*)mbuf_data(skb) + ehdrlen;

		if(checksumDemanded & DEMAND_IPv6){		// IPv6
			struct ip6_hdr* ip6 = (struct ip6_hdr*)packet;
			u_int8_t nexthdr;
			do {
				nexthdr = ip6->ip6_ctlun.ip6_un1.ip6_un1_nxt;
				ip6++;
			} while(nexthdr != IPPROTO_TCP && nexthdr != IPPROTO_UDP);
			ip_hlen = (u8*)ip6 - packet;
		} else {
			struct ip *ip = (struct ip *)packet;
			ip_hlen = ip->ip_hl << 2;
            if(ip_hlen == 0)
                ip_hlen = sizeof(struct ip);
			type_tucmd |= E1000_ADVTXD_TUCMD_IPV4;
		}
		vlan_macip_lens |= ip_hlen;
		
		if(checksumDemanded & IONetworkController::kChecksumTCP){
			type_tucmd |= E1000_ADVTXD_TUCMD_L4T_TCP;
			struct tcphdr* tcph = tcp_hdr(skb);
			mss_l4len_idx = (tcph->th_off << 2) << E1000_ADVTXD_L4LEN_SHIFT;
		} else if(checksumDemanded & CSUM_TCPIPv6){
			type_tucmd |= E1000_ADVTXD_TUCMD_L4T_TCP;
			struct tcphdr* tcph = tcp6_hdr(skb);
			mss_l4len_idx = (tcph->th_off << 2) << E1000_ADVTXD_L4LEN_SHIFT;
		} else if(checksumDemanded & (IONetworkController::kChecksumUDP|CSUM_UDPIPv6)){
			mss_l4len_idx = sizeof(struct udphdr) << E1000_ADVTXD_L4LEN_SHIFT;
		}
		
		first->tx_flags |= IGB_TX_FLAGS_CSUM;
	}
	vlan_macip_lens |= ehdrlen << E1000_ADVTXD_MACLEN_SHIFT;


#else	// __APPLE__
	
	if (skb->ip_summed != CHECKSUM_PARTIAL) {
		if (!(first->tx_flags & IGB_TX_FLAGS_VLAN))
			return;
	} else {
		u8 nexthdr = 0;
		switch (first->protocol) {
		case htons(ETH_P_IP):
			vlan_macip_lens |= skb_network_header_len(skb);
			type_tucmd |= E1000_ADVTXD_TUCMD_IPV4;
			nexthdr = ip_hdr(skb)->protocol;
			break;
#ifdef NETIF_F_IPV6_CSUM
		case htons(ETH_P_IPV6):
			vlan_macip_lens |= skb_network_header_len(skb);
			nexthdr = ipv6_hdr(skb)->nexthdr;
			break;
#endif
		default:
			if (unlikely(net_ratelimit())) {
				dev_warn(tx_ring->dev,
				 "partial checksum but proto=%x!\n",
				 first->protocol);
			}
			break;
		}

		switch (nexthdr) {
		case IPPROTO_TCP:
			type_tucmd |= E1000_ADVTXD_TUCMD_L4T_TCP;
			mss_l4len_idx = tcp_hdrlen(skb) <<
					E1000_ADVTXD_L4LEN_SHIFT;
			break;
#ifdef HAVE_SCTP
		case IPPROTO_SCTP:
			type_tucmd |= E1000_ADVTXD_TUCMD_L4T_SCTP;
			mss_l4len_idx = sizeof(struct sctphdr) <<
					E1000_ADVTXD_L4LEN_SHIFT;
			break;
#endif
		case IPPROTO_UDP:
			mss_l4len_idx = sizeof(struct udphdr) <<
					E1000_ADVTXD_L4LEN_SHIFT;
			break;
		default:
			if (unlikely(net_ratelimit())) {
				dev_warn(tx_ring->dev,
				 "partial checksum but l4 proto=%x!\n",
				 nexthdr);
			}
			break;
		}

		/* update TX checksum flag */
		first->tx_flags |= IGB_TX_FLAGS_CSUM;
	}

	vlan_macip_lens |= skb_network_offset(skb) << E1000_ADVTXD_MACLEN_SHIFT;
#endif
	vlan_macip_lens |= first->tx_flags & IGB_TX_FLAGS_VLAN_MASK;
	
	igb_tx_ctxtdesc(tx_ring, vlan_macip_lens, type_tucmd, mss_l4len_idx);
}

#define IGB_SET_FLAG(_input, _flag, _result) \
	((_flag <= _result) ? \
	((u32)(_input & _flag) * (_result / _flag)) : \
	((u32)(_input & _flag) / (_flag / _result)))

static u32 igb_tx_cmd_type(struct sk_buff *skb, u32 tx_flags)
{
	/* set type for advanced descriptor with frame checksum insertion */
	u32 cmd_type = E1000_ADVTXD_DTYP_DATA |
			E1000_ADVTXD_DCMD_DEXT |
			E1000_ADVTXD_DCMD_IFCS;

	/* set HW vlan bit if vlan is present */
	cmd_type |= IGB_SET_FLAG(tx_flags, IGB_TX_FLAGS_VLAN,
							 (E1000_ADVTXD_DCMD_VLE));

	/* set segmentation bits for TSO */
	cmd_type |= IGB_SET_FLAG(tx_flags, IGB_TX_FLAGS_TSO,
							 (E1000_ADVTXD_DCMD_TSE));

	/* set timestamp bit if present */
	cmd_type |= IGB_SET_FLAG(tx_flags, IGB_TX_FLAGS_TSTAMP,
							 (E1000_ADVTXD_MAC_TSTAMP));

	return cmd_type;
}

static void igb_tx_olinfo_status(struct igb_ring *tx_ring,
				 union e1000_adv_tx_desc *tx_desc,
				 u32 tx_flags, unsigned int paylen)
{
	u32 olinfo_status = paylen << E1000_ADVTXD_PAYLEN_SHIFT;
    
	/* 82575 requires a unique index per ring */
	if (test_bit(IGB_RING_FLAG_TX_CTX_IDX, &tx_ring->flags))
		olinfo_status |= tx_ring->reg_idx << 4;
    
	/* insert L4 checksum */
	olinfo_status |= IGB_SET_FLAG(tx_flags,
                                  IGB_TX_FLAGS_CSUM,
                                  (E1000_TXD_POPTS_TXSM << 8));
    
	/* insert IPv4 checksum */
	olinfo_status |= IGB_SET_FLAG(tx_flags,
                                  IGB_TX_FLAGS_IPV4,
                                  (E1000_TXD_POPTS_IXSM << 8));
    
	tx_desc->read.olinfo_status = cpu_to_le32(olinfo_status);
}

static bool igb_tx_map(struct igb_ring *tx_ring,
		       struct igb_tx_buffer *first,
		       const u8 hdr_len)
{
	struct sk_buff *skb = first->skb;
	struct igb_tx_buffer *tx_buffer;
	union e1000_adv_tx_desc *tx_desc;
	dma_addr_t dma;
#ifdef __APPLE__
	UInt32 k,count,frags;
	struct IOPhysicalSegment vec[MAX_SKB_FRAGS];
	frags = tx_ring->netdev->txCursor()->getPhysicalSegmentsWithCoalesce(skb, vec, MAX_SKB_FRAGS);
	if(frags == 0)
		return FALSE;

	// check real count
	count = 0;
	for (k = 0; k < frags; k++){
		count += (vec[k].length + (IGB_MAX_DATA_PER_TXD-1))/IGB_MAX_DATA_PER_TXD;
	}
	if (igb_desc_unused(tx_ring) < count + 3)
		return FALSE;
	
	
	unsigned int size;
#else	// __APPLE__
	unsigned int data_len, size;
#endif	// __APPLE__
	u32 tx_flags = first->tx_flags;
	u32 cmd_type = igb_tx_cmd_type(skb, tx_flags);
	u16 i = tx_ring->next_to_use;
	
	tx_desc = IGB_TX_DESC(tx_ring, i);
	
#ifdef __APPLE__
	igb_tx_olinfo_status(tx_ring, tx_desc, tx_flags, mbuf_pkthdr_len(skb) - hdr_len);

	dma = vec[0].location;
	size = vec[0].length;
#else
	igb_tx_olinfo_status(tx_ring, tx_desc, tx_flags, skb->len - hdr_len);
	
	size = skb_headlen(skb);
	data_len = skb->data_len;
	
	dma = dma_map_single(tx_ring->dev, skb->data, size, DMA_TO_DEVICE);
#endif

	tx_buffer = first;
	
#ifdef __APPLE__
	for (k=1;;k++) {
#else
	for (frag = &skb_shinfo(skb)->frags[0];; frag++) {
		if (dma_mapping_error(tx_ring->dev, dma))
			goto dma_error;
#endif
		/* record length, and DMA address */
		dma_unmap_len_set(tx_buffer, len, size);
		dma_unmap_addr_set(tx_buffer, dma, dma);
		
		tx_desc->read.buffer_addr = cpu_to_le64(dma);
		
		while (unlikely(size > IGB_MAX_DATA_PER_TXD)) {
			tx_desc->read.cmd_type_len =
			cpu_to_le32(cmd_type ^ IGB_MAX_DATA_PER_TXD);
			
			i++;
			tx_desc++;
			if (i == tx_ring->count) {
				tx_desc = IGB_TX_DESC(tx_ring, 0);
				i = 0;
			}
			tx_desc->read.olinfo_status = 0;
			
			dma += IGB_MAX_DATA_PER_TXD;
			size -= IGB_MAX_DATA_PER_TXD;
			
			tx_desc->read.buffer_addr = cpu_to_le64(dma);
		}
		
#ifdef __APPLE__
		if(k >= frags)
			break;
#else
		if (likely(!data_len))
			break;
#endif

		tx_desc->read.cmd_type_len = cpu_to_le32(cmd_type ^ size);
		
		i++;
		tx_desc++;
		if (i == tx_ring->count) {
			tx_desc = IGB_TX_DESC(tx_ring, 0);
			i = 0;
		}
		tx_desc->read.olinfo_status = 0;
		
#ifdef __APPLE__
		dma = vec[k].location;
		size = vec[k].length;
#else
		size = skb_frag_size(frag);
		data_len -= size;
		
		dma = skb_frag_dma_map(tx_ring->dev, frag, 0,
							   size, DMA_TO_DEVICE);
#endif
		
		tx_buffer = &tx_ring->tx_buffer_info[i];
	}
	
	/* write last descriptor with RS and EOP bits */
	cmd_type |= size | IGB_TXD_DCMD;
	tx_desc->read.cmd_type_len = cpu_to_le32(cmd_type);

#ifndef __APPLE__
	netdev_tx_sent_queue(txring_txq(tx_ring), first->bytecount);
#endif

	/* set the timestamp */
	first->time_stamp = jiffies;

	/*
	 * Force memory writes to complete before letting h/w know there
	 * are new descriptors to fetch.  (Only applicable for weak-ordered
	 * memory model archs, such as IA-64).
	 *
	 * We also need this memory barrier to make certain all of the
	 * status bits have been updated before next_to_watch is written.
	 */
	wmb();

	/* set next_to_watch value indicating a packet is present */
	first->next_to_watch = tx_desc;

	i++;
	if (i == tx_ring->count)
		i = 0;

	tx_ring->next_to_use = i;

	writel(i, tx_ring->tail);

	/* we need this if more than one processor can write to our tail
	 * at a time, it syncronizes IO on IA64/Altix systems */
	mmiowb();

	return TRUE;
#ifndef __APPLE__
dma_error:
	pr_err("TX DMA map failed\n");

	/* clear dma mappings for failed tx_buffer_info map */
	for (;;) {
		tx_buffer = &tx_ring->tx_buffer_info[i];
		igb_unmap_and_free_tx_resource(tx_ring, tx_buffer);
		if (tx_buffer == first)
			break;
		if (i == 0)
			i = tx_ring->count;
		i--;
	}

	tx_ring->next_to_use = i;
	return FALSE;
#endif
}

static int __igb_maybe_stop_tx(struct igb_ring *tx_ring, const u16 size)
{
#ifndef __APPLE__
	IOEthernetController *netdev = netdev_ring(tx_ring);

	if (netif_is_multiqueue(netdev))
		netif_stop_subqueue(netdev, ring_queue_index(tx_ring));
	else
		netif_stop_queue(netdev);

	/* Herbert's original patch had:
	 *  smp_mb__after_netif_stop_queue();
	 * but since that doesn't exist yet, just open code it.
	 */
	smp_mb();

	/* We need to check again in a case another CPU has just
	 * made room available.
	 */
	if (igb_desc_unused(tx_ring) < size)
		return -EBUSY;

	/* A reprieve! */
	if (netif_is_multiqueue(netdev))
		netif_wake_subqueue(netdev, ring_queue_index(tx_ring));
	else
		netif_wake_queue(netdev);

	tx_ring->tx_stats.restart_queue++;
	return 0;
#else
	return -EBUSY;
#endif
}

static inline int igb_maybe_stop_tx(struct igb_ring *tx_ring, const u16 size)
{
	if (igb_desc_unused(tx_ring) >= size)
		return 0;
	return __igb_maybe_stop_tx(tx_ring, size);
}

#ifndef __APPLE__	// see outputPacket()
netdev_tx_t igb_xmit_frame_ring(struct igb_adapter *adapter,struct sk_buff *skb,
				struct igb_ring *tx_ring)
{
	struct igb_tx_buffer *first;
	int tso;
	u32 tx_flags = 0;
#if PAGE_SIZE > IGB_MAX_DATA_PER_TXD
	unsigned short f;
#endif
	__be16 protocol = vlan_get_protocol(skb);
	u8 hdr_len = 0;
	/* need: 1 descriptor per page * PAGE_SIZE/IGB_MAX_DATA_PER_TXD,
	 *       + 1 desc for skb_headlen/IGB_MAX_DATA_PER_TXD,
	 *       + 2 desc gap to keep tail from touching head,
	 *       + 1 desc for context descriptor,
	 * otherwise try next time
	 */
#if PAGE_SIZE > IGB_MAX_DATA_PER_TXD
	for (f = 0; f < skb_shinfo(skb)->nr_frags; f++)
		count += TXD_USE_COUNT(skb_shinfo(skb)->frags[f].size);
#else
	count += skb_shinfo(skb)->nr_frags;
#endif
	if (igb_maybe_stop_tx(tx_ring, skb_shinfo(skb)->nr_frags + 4)) {
		/* this is a hard error */
		return NETDEV_TX_BUSY;
	}
	/* record the location of the first descriptor for this packet */
	first = &tx_ring->tx_buffer_info[tx_ring->next_to_use];
	first->skb = skb;
	first->bytecount = mbuf_pkthdr_len(skb);
	first->gso_segs = 1;

#ifdef HAVE_PTP_1588_CLOCK
#ifdef SKB_SHARED_TX_IS_UNION
	if (unlikely(skb_tx(skb)->hardware)) {
#else
		if (unlikely(skb_shinfo(skb)->tx_flags & SKBTX_HW_TSTAMP)) {
#endif
			struct igb_adapter *adapter = netdev_priv(tx_ring->netdev);
			
			if (!test_and_set_bit_lock(__IGB_PTP_TX_IN_PROGRESS,
									   &adapter->state)) {
#ifdef SKB_SHARED_TX_IS_UNION
				skb_tx(skb)->in_progress = 1;
#else
				skb_shinfo(skb)->tx_flags |= SKBTX_IN_PROGRESS;
#endif
			tx_flags |= IGB_TX_FLAGS_TSTAMP;
			
			adapter->ptp_tx_skb = skb_get(skb);
			adapter->ptp_tx_start = jiffies;
			if (adapter->hw.mac.type == e1000_82576)
				schedule_work(&adapter->ptp_tx_work);
		}
	}
#endif /* HAVE_PTP_1588_CLOCK */
	skb_tx_timestamp(skb);
	if (vlan_tx_tag_present(skb)) {
		tx_flags |= IGB_TX_FLAGS_VLAN;
		tx_flags |= (vlan_tx_tag_get(skb) << IGB_TX_FLAGS_VLAN_SHIFT);
	}

	/* record initial flags and protocol */
	first->tx_flags = tx_flags;
	first->protocol = protocol;
	
	tso = igb_tso(tx_ring, first, &hdr_len);
	if (tso < 0)
		goto out_drop;
	else if (!tso)
		igb_tx_csum(tx_ring, first);

	igb_tx_map(tx_ring, first, hdr_len);

#ifndef HAVE_TRANS_START_IN_QUEUE
	//netdev_ring(tx_ring)->trans_start = jiffies;

#endif
	/* Make sure there is space in the ring for the next send. */
	igb_maybe_stop_tx(tx_ring, DESC_NEEDED);

	return NETDEV_TX_OK;

out_drop:
	igb_unmap_and_free_tx_resource(adapter, tx_ring, first);

	return NETDEV_TX_OK;
}
#endif

#ifdef HAVE_TX_MQ
static inline struct igb_ring *igb_tx_queue_mapping(struct igb_adapter *adapter,
                                                    struct sk_buff *skb)
{
	unsigned int r_idx = skb->queue_mapping;

	if (r_idx >= adapter->num_tx_queues)
		r_idx = r_idx % adapter->num_tx_queues;

	return adapter->tx_ring[r_idx];
}
#else
#define igb_tx_queue_mapping(_adapter, _skb) ((_adapter)->tx_ring[0])
#endif

#ifndef __APPLE__	// see outputPacket()
static netdev_tx_t igb_xmit_frame(struct sk_buff *skb,
                                  IOEthernetController *netdev)
{
	struct igb_adapter *adapter = netdev_priv(netdev);

	if (test_bit(__IGB_DOWN, &adapter->state)) {
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}

	if (skb->len <= 0) {
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}

	/*
	 * The minimum packet size with TCTL.PSP set is 17 so pad the skb
	 * in order to meet this minimum size requirement.
	 */
	if (skb->len < 17) {
		if (skb_padto(skb, 17))
			return NETDEV_TX_OK;
		skb->len = 17;
	}

	return igb_xmit_frame_ring(skb, igb_tx_queue_mapping(adapter, skb));
}
#endif

/**
 * igb_tx_timeout - Respond to a Tx Hang
 * @netdev: network interface device structure
 **/
static void igb_tx_timeout(IOEthernetController *netdev)
{
	struct igb_adapter *adapter = netdev_priv(netdev);
	struct e1000_hw *hw = &adapter->hw;

	/* Do the reset outside of interrupt context */
	adapter->tx_timeout_count++;

	if (hw->mac.type >= e1000_82580)
		hw->dev_spec._82575.global_device_reset = true;

	schedule_work(&adapter->reset_task);
	E1000_WRITE_REG(hw, E1000_EICS,
			(adapter->eims_enable_mask & ~adapter->eims_other));
}

#ifndef	__APPLE__
static void igb_reset_task(struct work_struct *work)
{
	struct igb_adapter *adapter;
	adapter = container_of(work, struct igb_adapter, reset_task);

	igb_reinit_locked(adapter);
}
#endif

/**
 * igb_get_stats - Get System Network Statistics
 * @netdev: network interface device structure
 *
 * Returns the address of the device statistics structure.
 * The statistics are updated here and also from the timer callback.
 **/
static struct net_device_stats *igb_get_stats(IOEthernetController *netdev)
{
	struct igb_adapter *adapter = netdev_priv(netdev);
	
	if (!test_bit(__IGB_RESETTING, &adapter->state))
		igb_update_stats(adapter);
	
#ifdef HAVE_NETDEV_STATS_IN_NETDEV
	/* only return the current stats */
	return &netdev->stats;
#else
	/* only return the current stats */
	return &adapter->net_stats;
#endif /* HAVE_NETDEV_STATS_IN_NETDEV */
}

/**
 * igb_change_mtu - Change the Maximum Transfer Unit
 * @netdev: network interface device structure
 * @new_mtu: new value for maximum frame size
 *
 * Returns 0 on success, negative on failure
 **/
static int igb_change_mtu(IOEthernetController *netdev, int new_mtu)
{
	struct igb_adapter *adapter = netdev_priv(netdev);
	struct e1000_hw *hw = &adapter->hw;
	int max_frame = new_mtu + (ETH_HLEN + ETH_FCS_LEN); // + VLAN_HLEN

	/* adjust max frame to be at least the size of a standard frame */
	if (max_frame < (ETH_FRAME_LEN + ETH_FCS_LEN))
		max_frame = ETH_FRAME_LEN + ETH_FCS_LEN;

	while (test_and_set_bit(__IGB_RESETTING, &adapter->state))
		usleep_range(1000, 2000);

	/* igb_down has a dependency on max_frame_size */
	adapter->max_frame_size = max_frame;

	if (netif_running(netdev))
		igb_down(adapter);

	hw->dev_spec._82575.mtu = new_mtu;

	if (netif_running(netdev))
		igb_up(adapter);
	else
		igb_reset(adapter);

	clear_bit(__IGB_RESETTING, &adapter->state);

	return 0;
}

/**
 * igb_update_stats - Update the board statistics counters
 * @adapter: board private structure
 **/

void igb_update_stats(struct igb_adapter *adapter)
{
	IONetworkStats * net_stats = adapter->netdev->getNetStats();
	IOEthernetStats * ether_stats = adapter->netdev->getEtherStats();
	struct e1000_hw *hw = &adapter->hw;
#ifdef HAVE_PCI_ERS
	IOPCIDevice *pdev = adapter->pdev;
#endif
	u32 reg, mpc;
	u16 phy_tmp;
	int i;
	u64 bytes, packets;
#ifndef IGB_NO_LRO
	u32 flushed = 0, coal = 0;
	struct igb_q_vector *q_vector;
#endif
	
#define PHY_IDLE_ERROR_COUNT_MASK 0x00FF
	
	/*
	 * Prevent stats update while adapter is being reset, or if the pci
	 * connection is down.
	 */
	if (adapter->link_speed == 0)
		return;
#ifdef HAVE_PCI_ERS
	if (pci_channel_offline(pdev))
		return;
	
#endif
#ifndef IGB_NO_LRO
	for (i = 0; i < adapter->num_q_vectors; i++) {
		q_vector = adapter->q_vector[i];
		if (!q_vector)
			continue;
		flushed += q_vector->lrolist.stats.flushed;
		coal += q_vector->lrolist.stats.coal;
	}
	adapter->lro_stats.flushed = flushed;
	adapter->lro_stats.coal = coal;
	
#endif
	bytes = 0;
	packets = 0;
	for (i = 0; i < adapter->num_rx_queues; i++) {
		struct igb_ring *ring = adapter->rx_ring[i];
		u32 rqdpc_tmp = E1000_READ_REG(hw, E1000_RQDPC(i)) & 0x0FFF;
		if (hw->mac.type >= e1000_i210)
			E1000_WRITE_REG(hw, E1000_RQDPC(i), 0);
		ring->rx_stats.drops += rqdpc_tmp;
		ether_stats->dot3RxExtraEntry.overruns += rqdpc_tmp;
		
		bytes += ring->rx_stats.bytes;
		packets += ring->rx_stats.packets;
	}
	
	//net_stats->rx_bytes = bytes;
	net_stats->inputPackets = packets;
	
	bytes = 0;
	packets = 0;
	for (i = 0; i < adapter->num_tx_queues; i++) {
		struct igb_ring *ring = adapter->tx_ring[i];
		bytes += ring->tx_stats.bytes;
		packets += ring->tx_stats.packets;
	}
	//net_stats->tx_bytes = bytes;
	net_stats->outputPackets = packets;
	
	/* read stats registers */
	adapter->stats.crcerrs += E1000_READ_REG(hw, E1000_CRCERRS);
	adapter->stats.gprc += E1000_READ_REG(hw, E1000_GPRC);
	adapter->stats.gorc += E1000_READ_REG(hw, E1000_GORCL);
	E1000_READ_REG(hw, E1000_GORCH); /* clear GORCL */
	adapter->stats.bprc += E1000_READ_REG(hw, E1000_BPRC);
	adapter->stats.mprc += E1000_READ_REG(hw, E1000_MPRC);
	adapter->stats.roc += E1000_READ_REG(hw, E1000_ROC);
	
	adapter->stats.prc64 += E1000_READ_REG(hw, E1000_PRC64);
	adapter->stats.prc127 += E1000_READ_REG(hw, E1000_PRC127);
	adapter->stats.prc255 += E1000_READ_REG(hw, E1000_PRC255);
	adapter->stats.prc511 += E1000_READ_REG(hw, E1000_PRC511);
	adapter->stats.prc1023 += E1000_READ_REG(hw, E1000_PRC1023);
	adapter->stats.prc1522 += E1000_READ_REG(hw, E1000_PRC1522);
	adapter->stats.symerrs += E1000_READ_REG(hw, E1000_SYMERRS);
	adapter->stats.sec += E1000_READ_REG(hw, E1000_SEC);
	
	mpc = E1000_READ_REG(hw, E1000_MPC);
	adapter->stats.mpc += mpc;
	ether_stats->dot3RxExtraEntry.overruns += mpc;
	adapter->stats.scc += E1000_READ_REG(hw, E1000_SCC);
	adapter->stats.ecol += E1000_READ_REG(hw, E1000_ECOL);
	adapter->stats.mcc += E1000_READ_REG(hw, E1000_MCC);
	adapter->stats.latecol += E1000_READ_REG(hw, E1000_LATECOL);
	adapter->stats.dc += E1000_READ_REG(hw, E1000_DC);
	adapter->stats.rlec += E1000_READ_REG(hw, E1000_RLEC);
	adapter->stats.xonrxc += E1000_READ_REG(hw, E1000_XONRXC);
	adapter->stats.xontxc += E1000_READ_REG(hw, E1000_XONTXC);
	adapter->stats.xoffrxc += E1000_READ_REG(hw, E1000_XOFFRXC);
	adapter->stats.xofftxc += E1000_READ_REG(hw, E1000_XOFFTXC);
	adapter->stats.fcruc += E1000_READ_REG(hw, E1000_FCRUC);
	adapter->stats.gptc += E1000_READ_REG(hw, E1000_GPTC);
	adapter->stats.gotc += E1000_READ_REG(hw, E1000_GOTCL);
	E1000_READ_REG(hw, E1000_GOTCH); /* clear GOTCL */
	adapter->stats.rnbc += E1000_READ_REG(hw, E1000_RNBC);
	adapter->stats.ruc += E1000_READ_REG(hw, E1000_RUC);
	adapter->stats.rfc += E1000_READ_REG(hw, E1000_RFC);
	adapter->stats.rjc += E1000_READ_REG(hw, E1000_RJC);
	adapter->stats.tor += E1000_READ_REG(hw, E1000_TORH);
	adapter->stats.tot += E1000_READ_REG(hw, E1000_TOTH);
	adapter->stats.tpr += E1000_READ_REG(hw, E1000_TPR);
	
	adapter->stats.ptc64 += E1000_READ_REG(hw, E1000_PTC64);
	adapter->stats.ptc127 += E1000_READ_REG(hw, E1000_PTC127);
	adapter->stats.ptc255 += E1000_READ_REG(hw, E1000_PTC255);
	adapter->stats.ptc511 += E1000_READ_REG(hw, E1000_PTC511);
	adapter->stats.ptc1023 += E1000_READ_REG(hw, E1000_PTC1023);
	adapter->stats.ptc1522 += E1000_READ_REG(hw, E1000_PTC1522);
	
	adapter->stats.mptc += E1000_READ_REG(hw, E1000_MPTC);
	adapter->stats.bptc += E1000_READ_REG(hw, E1000_BPTC);
	
	adapter->stats.tpt += E1000_READ_REG(hw, E1000_TPT);
	adapter->stats.colc += E1000_READ_REG(hw, E1000_COLC);
	
	adapter->stats.algnerrc += E1000_READ_REG(hw, E1000_ALGNERRC);
	/* read internal phy sepecific stats */
	reg = E1000_READ_REG(hw, E1000_CTRL_EXT);
	if (!(reg & E1000_CTRL_EXT_LINK_MODE_MASK)) {
		adapter->stats.rxerrc += E1000_READ_REG(hw, E1000_RXERRC);
		
		/* this stat has invalid values on i210/i211 */
		if ((hw->mac.type != e1000_i210) &&
			(hw->mac.type != e1000_i211))
			adapter->stats.tncrs += E1000_READ_REG(hw, E1000_TNCRS);
	}
	
	adapter->stats.tsctc += E1000_READ_REG(hw, E1000_TSCTC);
	adapter->stats.tsctfc += E1000_READ_REG(hw, E1000_TSCTFC);
	
	adapter->stats.iac += E1000_READ_REG(hw, E1000_IAC);
	adapter->stats.icrxoc += E1000_READ_REG(hw, E1000_ICRXOC);
	adapter->stats.icrxptc += E1000_READ_REG(hw, E1000_ICRXPTC);
	adapter->stats.icrxatc += E1000_READ_REG(hw, E1000_ICRXATC);
	adapter->stats.ictxptc += E1000_READ_REG(hw, E1000_ICTXPTC);
	adapter->stats.ictxatc += E1000_READ_REG(hw, E1000_ICTXATC);
	adapter->stats.ictxqec += E1000_READ_REG(hw, E1000_ICTXQEC);
	adapter->stats.ictxqmtc += E1000_READ_REG(hw, E1000_ICTXQMTC);
	adapter->stats.icrxdmtc += E1000_READ_REG(hw, E1000_ICRXDMTC);
	
	/* Fill out the OS statistics structure */
	//net_stats->multicast = adapter->stats.mprc;
	net_stats->collisions = (u32)adapter->stats.colc;
	
	/* Rx Errors */
	
	/* RLEC on some newer hardware can be incorrect so build
	 * our own version based on RUC and ROC
	 */
	net_stats->inputErrors = (u32)(adapter->stats.rxerrc +
        adapter->stats.crcerrs + adapter->stats.algnerrc +
        adapter->stats.ruc + adapter->stats.roc +
        adapter->stats.cexterr);
	ether_stats->dot3StatsEntry.frameTooLongs = (u32)adapter->stats.roc;
	ether_stats->dot3RxExtraEntry.frameTooShorts = (u32)adapter->stats.ruc;
	ether_stats->dot3StatsEntry.fcsErrors = (u32)adapter->stats.crcerrs;
	ether_stats->dot3StatsEntry.alignmentErrors = (u32)adapter->stats.algnerrc;
	ether_stats->dot3StatsEntry.missedFrames = (u32)adapter->stats.mpc;
	
	/* Tx Errors */
	net_stats->outputErrors = (u32)(adapter->stats.ecol + adapter->stats.latecol);
	ether_stats->dot3StatsEntry.deferredTransmissions = (u32)adapter->stats.ecol;
	ether_stats->dot3StatsEntry.lateCollisions = (u32)adapter->stats.latecol;
	ether_stats->dot3StatsEntry.carrierSenseErrors = (u32)adapter->stats.tncrs;
	
	/* Tx Dropped needs to be maintained elsewhere */
	
	/* Phy Stats */
	if (hw->phy.media_type == e1000_media_type_copper) {
		if ((adapter->link_speed == SPEED_1000) &&
			(!e1000_read_phy_reg(hw, PHY_1000T_STATUS, &phy_tmp))) {
			phy_tmp &= PHY_IDLE_ERROR_COUNT_MASK;
			adapter->phy_stats.idle_errors += phy_tmp;
		}
	}
	
	/* Management Stats */
	adapter->stats.mgptc += E1000_READ_REG(hw, E1000_MGTPTC);
	adapter->stats.mgprc += E1000_READ_REG(hw, E1000_MGTPRC);
	if (hw->mac.type > e1000_82580) {
		adapter->stats.o2bgptc += E1000_READ_REG(hw, E1000_O2BGPTC);
		adapter->stats.o2bspc += E1000_READ_REG(hw, E1000_O2BSPC);
		adapter->stats.b2ospc += E1000_READ_REG(hw, E1000_B2OSPC);
		adapter->stats.b2ogprc += E1000_READ_REG(hw, E1000_B2OGPRC);
	}
}

	
#ifndef __APPLE__
static irqreturn_t igb_msix_other(int irq, void *data)
{
	struct igb_adapter *adapter = data;
	struct e1000_hw *hw = &adapter->hw;
	u32 icr = E1000_READ_REG(hw, E1000_ICR);
	/* reading ICR causes bit 31 of EICR to be cleared */

	if (icr & E1000_ICR_DRSTA)
		schedule_work(&adapter->reset_task);

	if (icr & E1000_ICR_DOUTSYNC) {
		/* HW is reporting DMA is out of sync */
		adapter->stats.doosync++;
		/* The DMA Out of Sync is also indication of a spoof event
		 * in IOV mode. Check the Wrong VM Behavior register to
		 * see if it is really a spoof event.
		 */
		igb_check_wvbr(adapter);
	}

	/* Check for a mailbox event */
	if (icr & E1000_ICR_VMMB)
		igb_msg_task(adapter);

	if (icr & E1000_ICR_LSC) {
		hw->mac.get_link_status = 1;
		/* guard against interrupt when we're going down */
		if (!test_bit(__IGB_DOWN, &adapter->state))
			mod_timer(&adapter->watchdog_timer, jiffies + 1);
	}

#ifdef HAVE_PTP_1588_CLOCK
	if (icr & E1000_ICR_TS) {
		u32 tsicr = E1000_READ_REG(hw, E1000_TSICR);
		
		if (tsicr & E1000_TSICR_TXTS) {
			/* acknowledge the interrupt */
			E1000_WRITE_REG(hw, E1000_TSICR, E1000_TSICR_TXTS);
			/* retrieve hardware timestamp */
			schedule_work(&adapter->ptp_tx_work);
		}
	}
#endif /* HAVE_PTP_1588_CLOCK */

	/* Check for MDD event */
	if (icr & E1000_ICR_MDDET)
		igb_process_mdd_event(adapter);

	E1000_WRITE_REG(hw, E1000_EIMS, adapter->eims_other);

	return IRQ_HANDLED;
}
#endif

static void igb_write_itr(struct igb_q_vector *q_vector)
{
	struct igb_adapter *adapter = q_vector->adapter;
	u32 itr_val = q_vector->itr_val & 0x7FFC;

	if (!q_vector->set_itr)
		return;

	if (!itr_val)
		itr_val = 0x4;

	if (adapter->hw.mac.type == e1000_82575)
		itr_val |= itr_val << 16;
	else
		itr_val |= E1000_EITR_CNT_IGNR;

	writel(itr_val, q_vector->itr_register);
	q_vector->set_itr = 0;
}

#ifndef __APPLE__
static irqreturn_t igb_msix_ring(int irq, void *data)
{
	struct igb_q_vector *q_vector = data;

	/* Write the ITR value calculated from the previous interrupt. */
	igb_write_itr(q_vector);

	napi_schedule(&q_vector->napi);

	return IRQ_HANDLED;
}
#endif

#ifdef IGB_DCA
	static void igb_update_tx_dca(struct igb_adapter *adapter,
								  struct igb_ring *tx_ring,
								  int cpu)
{
	struct e1000_hw *hw = &adapter->hw;
	u32 txctrl = dca3_get_tag(tx_ring->dev, cpu);
	
	if (hw->mac.type != e1000_82575)
		txctrl <<= E1000_DCA_TXCTRL_CPUID_SHIFT_82576;
	
	/*
	 * We can enable relaxed ordering for reads, but not writes when
	 * DCA is enabled.  This is due to a known issue in some chipsets
	 * which will cause the DCA tag to be cleared.
	 */
	txctrl |= E1000_DCA_TXCTRL_DESC_RRO_EN |
	E1000_DCA_TXCTRL_DATA_RRO_EN |
	E1000_DCA_TXCTRL_DESC_DCA_EN;
	
	E1000_WRITE_REG(hw, E1000_DCA_TXCTRL(tx_ring->reg_idx), txctrl);
}
	
	static void igb_update_rx_dca(struct igb_adapter *adapter,
								  struct igb_ring *rx_ring,
								  int cpu)
{
	struct e1000_hw *hw = &adapter->hw;
	u32 rxctrl = dca3_get_tag(&adapter->pdev->dev, cpu);
	
	if (hw->mac.type != e1000_82575)
		rxctrl <<= E1000_DCA_RXCTRL_CPUID_SHIFT_82576;
	
	/*
	 * We can enable relaxed ordering for reads, but not writes when
	 * DCA is enabled.  This is due to a known issue in some chipsets
	 * which will cause the DCA tag to be cleared.
	 */
	rxctrl |= E1000_DCA_RXCTRL_DESC_RRO_EN |
	E1000_DCA_RXCTRL_DESC_DCA_EN;
	
	E1000_WRITE_REG(hw, E1000_DCA_RXCTRL(rx_ring->reg_idx), rxctrl);
}
		
static void igb_update_dca(struct igb_q_vector *q_vector)
{
	struct igb_adapter *adapter = q_vector->adapter;
	int cpu = get_cpu();
	
	if (q_vector->cpu == cpu)
		goto out_no_update;
	
	if (q_vector->tx.ring)
		igb_update_tx_dca(adapter, q_vector->tx.ring, cpu);
	
	if (q_vector->rx.ring)
		igb_update_rx_dca(adapter, q_vector->rx.ring, cpu);
	
	q_vector->cpu = cpu;
out_no_update:
	put_cpu();
}

static void igb_setup_dca(struct igb_adapter *adapter)
{
	struct e1000_hw *hw = &adapter->hw;
	int i;

	if (!(adapter->flags & IGB_FLAG_DCA_ENABLED))
		return;

	/* Always use CB2 mode, difference is masked in the CB driver. */
	E1000_WRITE_REG(hw, E1000_DCA_CTRL, E1000_DCA_CTRL_DCA_MODE_CB2);

	for (i = 0; i < adapter->num_q_vectors; i++) {
		adapter->q_vector[i]->cpu = -1;
		igb_update_dca(adapter->q_vector[i]);
	}
}

static int __igb_notify_dca(struct device *dev, void *data)
{
	IOEthernetController *netdev = dev_get_drvdata(dev);
	struct igb_adapter *adapter = netdev_priv(netdev);
	IOPCIDevice *pdev = adapter->pdev;
	struct e1000_hw *hw = &adapter->hw;
	unsigned long event = *(unsigned long *)data;

	switch (event) {
	case DCA_PROVIDER_ADD:
		/* if already enabled, don't do it again */
		if (adapter->flags & IGB_FLAG_DCA_ENABLED)
			break;
		if (dca_add_requester(dev) == E1000_SUCCESS) {
			adapter->flags |= IGB_FLAG_DCA_ENABLED;
			dev_info(pci_dev_to_dev(pdev), "DCA enabled\n");
			igb_setup_dca(adapter);
			break;
		}
		/* Fall Through since DCA is disabled. */
	case DCA_PROVIDER_REMOVE:
		if (adapter->flags & IGB_FLAG_DCA_ENABLED) {
			/* without this a class_device is left
			 * hanging around in the sysfs model */
			dca_remove_requester(dev);
			dev_info(pci_dev_to_dev(pdev), "DCA disabled\n");
			adapter->flags &= ~IGB_FLAG_DCA_ENABLED;
			E1000_WRITE_REG(hw, E1000_DCA_CTRL, E1000_DCA_CTRL_DCA_DISABLE);
		}
		break;
	}

	return E1000_SUCCESS;
}

static int igb_notify_dca(struct notifier_block *nb, unsigned long event,
                          void *p)
{
	int ret_val;

	ret_val = driver_for_each_device(&igb_driver.driver, NULL, &event,
	                                 __igb_notify_dca);

	return ret_val ? NOTIFY_BAD : NOTIFY_DONE;
}
#endif /* IGB_DCA */

static int igb_vf_configure(struct igb_adapter *adapter, int vf)
{
	unsigned char mac_addr[ETH_ALEN];
	
	random_ether_addr(mac_addr);
	igb_set_vf_mac(adapter, vf, mac_addr);
	
#ifdef IFLA_VF_MAX
#ifdef HAVE_VF_SPOOFCHK_CONFIGURE
	/* By default spoof check is enabled for all VFs */
	adapter->vf_data[vf].spoofchk_enabled = true;
#endif
#endif
	return true;
}
	
	
static void igb_ping_all_vfs(struct igb_adapter *adapter)
{
	struct e1000_hw *hw = &adapter->hw;
	u32 ping;
	int i;

	for (i = 0 ; i < adapter->vfs_allocated_count; i++) {
		ping = E1000_PF_CONTROL_MSG;
		if (adapter->vf_data[i].flags & IGB_VF_FLAG_CTS)
			ping |= E1000_VT_MSGTYPE_CTS;
		e1000_write_mbx(hw, &ping, 1, i);
	}
}

/**
 *  igb_mta_set_ - Set multicast filter table address
 *  @adapter: pointer to the adapter structure
 *  @hash_value: determines the MTA register and bit to set
 *
 *  The multicast table address is a register array of 32-bit registers.
 *  The hash_value is used to determine what register the bit is in, the
 *  current value is read, the new bit is OR'd in and the new value is
 *  written back into the register.
 **/
void igb_mta_set(struct igb_adapter *adapter, u32 hash_value)
{
	struct e1000_hw *hw = &adapter->hw;
	u32 hash_bit, hash_reg, mta;

	/*
	 * The MTA is a register array of 32-bit registers. It is
	 * treated like an array of (32*mta_reg_count) bits.  We want to
	 * set bit BitArray[hash_value]. So we figure out what register
	 * the bit is in, read it, OR in the new bit, then write
	 * back the new value.  The (hw->mac.mta_reg_count - 1) serves as a
	 * mask to bits 31:5 of the hash value which gives us the
	 * register we're modifying.  The hash bit within that register
	 * is determined by the lower 5 bits of the hash value.
	 */
	hash_reg = (hash_value >> 5) & (hw->mac.mta_reg_count - 1);
	hash_bit = hash_value & 0x1F;

	mta = E1000_READ_REG_ARRAY(hw, E1000_MTA, hash_reg);

	mta |= (1 << hash_bit);

	E1000_WRITE_REG_ARRAY(hw, E1000_MTA, hash_reg, mta);
	E1000_WRITE_FLUSH(hw);
}

static int igb_set_vf_promisc(struct igb_adapter *adapter, u32 *msgbuf, u32 vf)
{

	struct e1000_hw *hw = &adapter->hw;
	u32 vmolr = E1000_READ_REG(hw, E1000_VMOLR(vf));
	struct vf_data_storage *vf_data = &adapter->vf_data[vf];

	vf_data->flags &= ~(IGB_VF_FLAG_UNI_PROMISC |
	                    IGB_VF_FLAG_MULTI_PROMISC);
	vmolr &= ~(E1000_VMOLR_ROPE | E1000_VMOLR_ROMPE | E1000_VMOLR_MPME);

#ifdef IGB_ENABLE_VF_PROMISC
	if (*msgbuf & E1000_VF_SET_PROMISC_UNICAST) {
		vmolr |= E1000_VMOLR_ROPE;
		vf_data->flags |= IGB_VF_FLAG_UNI_PROMISC;
		*msgbuf &= ~E1000_VF_SET_PROMISC_UNICAST;
	}
#endif
	if (*msgbuf & E1000_VF_SET_PROMISC_MULTICAST) {
		vmolr |= E1000_VMOLR_MPME;
		vf_data->flags |= IGB_VF_FLAG_MULTI_PROMISC;
		*msgbuf &= ~E1000_VF_SET_PROMISC_MULTICAST;
	} else {
		/*
		 * if we have hashes and we are clearing a multicast promisc
		 * flag we need to write the hashes to the MTA as this step
		 * was previously skipped
		 */
		if (vf_data->num_vf_mc_hashes > 30) {
			vmolr |= E1000_VMOLR_MPME;
		} else if (vf_data->num_vf_mc_hashes) {
			int j;
			vmolr |= E1000_VMOLR_ROMPE;
			for (j = 0; j < vf_data->num_vf_mc_hashes; j++)
				igb_mta_set(adapter, vf_data->vf_mc_hashes[j]);
		}
	}

	E1000_WRITE_REG(hw, E1000_VMOLR(vf), vmolr);

	/* there are flags left unprocessed, likely not supported */
	if (*msgbuf & E1000_VT_MSGINFO_MASK)
		return -EINVAL;

	return 0;

}

static int igb_set_vf_multicasts(struct igb_adapter *adapter,
				  u32 *msgbuf, u32 vf)
{
	int n = (msgbuf[0] & E1000_VT_MSGINFO_MASK) >> E1000_VT_MSGINFO_SHIFT;
	u16 *hash_list = (u16 *)&msgbuf[1];
	struct vf_data_storage *vf_data = &adapter->vf_data[vf];
	int i;

	/* salt away the number of multicast addresses assigned
	 * to this VF for later use to restore when the PF multi cast
	 * list changes
	 */
	vf_data->num_vf_mc_hashes = n;

	/* only up to 30 hash values supported */
	if (n > 30)
		n = 30;

	/* store the hashes for later use */
	for (i = 0; i < n; i++)
		vf_data->vf_mc_hashes[i] = hash_list[i];

	/* Flush and reset the mta with the new values */
	igb_set_rx_mode(adapter->netdev);

	return 0;
}

static void igb_restore_vf_multicasts(struct igb_adapter *adapter)
{
	struct e1000_hw *hw = &adapter->hw;
	struct vf_data_storage *vf_data;
	int i, j;

	for (i = 0; i < adapter->vfs_allocated_count; i++) {
		u32 vmolr = E1000_READ_REG(hw, E1000_VMOLR(i));
		vmolr &= ~(E1000_VMOLR_ROMPE | E1000_VMOLR_MPME);

		vf_data = &adapter->vf_data[i];

		if ((vf_data->num_vf_mc_hashes > 30) ||
		    (vf_data->flags & IGB_VF_FLAG_MULTI_PROMISC)) {
			vmolr |= E1000_VMOLR_MPME;
		} else if (vf_data->num_vf_mc_hashes) {
			vmolr |= E1000_VMOLR_ROMPE;
			for (j = 0; j < vf_data->num_vf_mc_hashes; j++)
				igb_mta_set(adapter, vf_data->vf_mc_hashes[j]);
		}
		E1000_WRITE_REG(hw, E1000_VMOLR(i), vmolr);
	}
}

static void igb_clear_vf_vfta(struct igb_adapter *adapter, u32 vf)
{
	struct e1000_hw *hw = &adapter->hw;
	u32 pool_mask, reg, vid;
	u16 vlan_default;
	int i;

	pool_mask = 1 << (E1000_VLVF_POOLSEL_SHIFT + vf);

	/* Find the vlan filter for this id */
	for (i = 0; i < E1000_VLVF_ARRAY_SIZE; i++) {
		reg = E1000_READ_REG(hw, E1000_VLVF(i));

		/* remove the vf from the pool */
		reg &= ~pool_mask;

		/* if pool is empty then remove entry from vfta */
		if (!(reg & E1000_VLVF_POOLSEL_MASK) &&
		    (reg & E1000_VLVF_VLANID_ENABLE)) {
			reg = 0;
			vid = reg & E1000_VLVF_VLANID_MASK;
			igb_vfta_set(adapter, vid, FALSE);
		}

		E1000_WRITE_REG(hw, E1000_VLVF(i), reg);
	}

	adapter->vf_data[vf].vlans_enabled = 0;

	vlan_default = adapter->vf_data[vf].default_vf_vlan_id;
	if (vlan_default)
		igb_vlvf_set(adapter, vlan_default, true, vf);
}

s32 igb_vlvf_set(struct igb_adapter *adapter, u32 vid, bool add, u32 vf)
{
	struct e1000_hw *hw = &adapter->hw;
	u32 reg = 0, i;

	/* The vlvf table only exists on 82576 hardware and newer */
	if (hw->mac.type < e1000_82576)
		return -1;

	/* we only need to do this if VMDq is enabled */
	if (!adapter->vmdq_pools)
		return -1;

	/* Find the vlan filter for this id */
	for (i = 0; i < E1000_VLVF_ARRAY_SIZE; i++) {
		reg = E1000_READ_REG(hw, E1000_VLVF(i));
		if ((reg & E1000_VLVF_VLANID_ENABLE) &&
		    vid == (reg & E1000_VLVF_VLANID_MASK))
			break;
	}

	if (add) {
		if (i == E1000_VLVF_ARRAY_SIZE) {
			/* Did not find a matching VLAN ID entry that was
			 * enabled.  Search for a free filter entry, i.e.
			 * one without the enable bit set
			 */
			for (i = 0; i < E1000_VLVF_ARRAY_SIZE; i++) {
				reg = E1000_READ_REG(hw, E1000_VLVF(i));
				if (!(reg & E1000_VLVF_VLANID_ENABLE))
					break;
			}
		}
		if (i < E1000_VLVF_ARRAY_SIZE) {
			/* Found an enabled/available entry */
			reg |= 1 << (E1000_VLVF_POOLSEL_SHIFT + vf);

			/* if !enabled we need to set this up in vfta */
			if (!(reg & E1000_VLVF_VLANID_ENABLE)) {
				/* add VID to filter table */
				igb_vfta_set(adapter, vid, TRUE);
				reg |= E1000_VLVF_VLANID_ENABLE;
			}
			reg &= ~E1000_VLVF_VLANID_MASK;
			reg |= vid;
			E1000_WRITE_REG(hw, E1000_VLVF(i), reg);

			/* do not modify RLPML for PF devices */
			if (vf >= adapter->vfs_allocated_count)
				return E1000_SUCCESS;

			if (!adapter->vf_data[vf].vlans_enabled) {
				u32 size;
				reg = E1000_READ_REG(hw, E1000_VMOLR(vf));
				size = reg & E1000_VMOLR_RLPML_MASK;
				size += 4;
				reg &= ~E1000_VMOLR_RLPML_MASK;
				reg |= size;
				E1000_WRITE_REG(hw, E1000_VMOLR(vf), reg);
			}

			adapter->vf_data[vf].vlans_enabled++;
		}
	} else {
		if (i < E1000_VLVF_ARRAY_SIZE) {
			/* remove vf from the pool */
			reg &= ~(1 << (E1000_VLVF_POOLSEL_SHIFT + vf));
			/* if pool is empty then remove entry from vfta */
			if (!(reg & E1000_VLVF_POOLSEL_MASK)) {
				reg = 0;
				igb_vfta_set(adapter, vid, FALSE);
			}
			E1000_WRITE_REG(hw, E1000_VLVF(i), reg);

			/* do not modify RLPML for PF devices */
			if (vf >= adapter->vfs_allocated_count)
				return E1000_SUCCESS;

			adapter->vf_data[vf].vlans_enabled--;
			if (!adapter->vf_data[vf].vlans_enabled) {
				u32 size;
				reg = E1000_READ_REG(hw, E1000_VMOLR(vf));
				size = reg & E1000_VMOLR_RLPML_MASK;
				size -= 4;
				reg &= ~E1000_VMOLR_RLPML_MASK;
				reg |= size;
				E1000_WRITE_REG(hw, E1000_VMOLR(vf), reg);
			}
		}
	}
	return E1000_SUCCESS;
}

#ifdef IFLA_VF_MAX
static void igb_set_vmvir(struct igb_adapter *adapter, u32 vid, u32 vf)
{
	struct e1000_hw *hw = &adapter->hw;

	if (vid)
		E1000_WRITE_REG(hw, E1000_VMVIR(vf), (vid | E1000_VMVIR_VLANA_DEFAULT));
	else
		E1000_WRITE_REG(hw, E1000_VMVIR(vf), 0);
}

static int igb_ndo_set_vf_vlan(IOEthernetController *netdev,
			       int vf, u16 vlan, u8 qos)
{
	int err = 0;
	struct igb_adapter *adapter = netdev_priv(netdev);

	/* VLAN IDs accepted range 0-4094 */
	if ((vf >= adapter->vfs_allocated_count) || (vlan > VLAN_VID_MASK-1) || (qos > 7))
		return -EINVAL;
	if (vlan || qos) {
		err = igb_vlvf_set(adapter, vlan, !!vlan, vf);
		if (err)
			goto out;
		igb_set_vmvir(adapter, vlan | (qos << VLAN_PRIO_SHIFT), vf);
		igb_set_vmolr(adapter, vf, !vlan);
		adapter->vf_data[vf].pf_vlan = vlan;
		adapter->vf_data[vf].pf_qos = qos;
		igb_set_vf_vlan_strip(adapter, vf, true);
		dev_info(&adapter->pdev->dev,
			 "Setting VLAN %d, QOS 0x%x on VF %d\n", vlan, qos, vf);
		if (test_bit(__IGB_DOWN, &adapter->state)) {
			dev_warn(&adapter->pdev->dev,
				 "The VF VLAN has been set, but the PF device is not up.\n");
			dev_warn(&adapter->pdev->dev,
				 "Bring the PF device up before attempting to use the VF device.\n");
		}
	} else {
		if (adapter->vf_data[vf].pf_vlan)
			dev_info(&adapter->pdev->dev,
					 "Clearing VLAN on VF %d\n", vf);
		igb_vlvf_set(adapter, adapter->vf_data[vf].pf_vlan,
				   false, vf);
		igb_set_vmvir(adapter, vlan, vf);
		igb_set_vmolr(adapter, vf, true);
		igb_set_vf_vlan_strip(adapter, vf, false);
		adapter->vf_data[vf].pf_vlan = 0;
		adapter->vf_data[vf].pf_qos = 0;
       }
out:
       return err;
}

#ifdef HAVE_VF_SPOOFCHK_CONFIGURE
static int igb_ndo_set_vf_spoofchk(struct net_device *netdev, int vf,
									bool setting)
{
	struct igb_adapter *adapter = netdev_priv(netdev);
	struct e1000_hw *hw = &adapter->hw;
	u32 dtxswc, reg_offset;
	
	if (!adapter->vfs_allocated_count)
		return -EOPNOTSUPP;
	
	if (vf >= adapter->vfs_allocated_count)
		return -EINVAL;
	
	reg_offset = (hw->mac.type == e1000_82576) ? E1000_DTXSWC : E1000_TXSWC;
	dtxswc = E1000_READ_REG(hw, reg_offset);
	if (setting)
		dtxswc |= ((1 << vf) |
				   (1 << (vf + E1000_DTXSWC_VLAN_SPOOF_SHIFT)));
	else
		dtxswc &= ~((1 << vf) |
					(1 << (vf + E1000_DTXSWC_VLAN_SPOOF_SHIFT)));
	E1000_WRITE_REG(hw, reg_offset, dtxswc);
	
	adapter->vf_data[vf].spoofchk_enabled = setting;
	return E1000_SUCCESS;
}
#endif /* HAVE_VF_SPOOFCHK_CONFIGURE */
#endif

static int igb_find_vlvf_entry(struct igb_adapter *adapter, int vid)
{
	struct e1000_hw *hw = &adapter->hw;
	int i;
	u32 reg;
	
	/* Find the vlan filter for this id */
	for (i = 0; i < E1000_VLVF_ARRAY_SIZE; i++) {
		reg = E1000_READ_REG(hw, E1000_VLVF(i));
		if ((reg & E1000_VLVF_VLANID_ENABLE) &&
			vid == (reg & E1000_VLVF_VLANID_MASK))
			break;
	}
	
	if (i >= E1000_VLVF_ARRAY_SIZE)
		i = -1;
	
	return i;
}
	

static int igb_set_vf_vlan(struct igb_adapter *adapter, u32 *msgbuf, u32 vf)
{
	struct e1000_hw *hw = &adapter->hw;
	int add = (msgbuf[0] & E1000_VT_MSGINFO_MASK) >> E1000_VT_MSGINFO_SHIFT;
	int vid = (msgbuf[1] & E1000_VLVF_VLANID_MASK);
	int err = 0;
	
	if (vid)
		igb_set_vf_vlan_strip(adapter, vf, true);
	else
		igb_set_vf_vlan_strip(adapter, vf, false);
	
	/* If in promiscuous mode we need to make sure the PF also has
	 * the VLAN filter set.
	 */
	if (add && (((AppleIGB*)(adapter->netdev))->flags() & IFF_PROMISC))
		err = igb_vlvf_set(adapter, vid, add,
						   adapter->vfs_allocated_count);
	if (err)
		goto out;
	
	err = igb_vlvf_set(adapter, vid, add, vf);
	
	if (err)
		goto out;
	
	/* Go through all the checks to see if the VLAN filter should
	 * be wiped completely.
	 */
	if (!add && (((AppleIGB*)(adapter->netdev))->flags() & IFF_PROMISC)) {
		u32 vlvf, bits;
		
		int regndx = igb_find_vlvf_entry(adapter, vid);
		if (regndx < 0)
			goto out;
		/* See if any other pools are set for this VLAN filter
		 * entry other than the PF.
		 */
		vlvf = bits = E1000_READ_REG(hw, E1000_VLVF(regndx));
		bits &= 1 << (E1000_VLVF_POOLSEL_SHIFT +
					  adapter->vfs_allocated_count);
		/* If the filter was removed then ensure PF pool bit
		 * is cleared if the PF only added itself to the pool
		 * because the PF is in promiscuous mode.
		 */
		if ((vlvf & VLAN_VID_MASK) == vid &&
#ifndef HAVE_VLAN_RX_REGISTER
		    !test_bit(vid, adapter->active_vlans) &&
#endif
		    !bits)
			igb_vlvf_set(adapter, vid, add,
						 adapter->vfs_allocated_count);
	}
	
out:
	return err;
}

static inline void igb_vf_reset(struct igb_adapter *adapter, u32 vf)
{
	struct e1000_hw *hw = &adapter->hw;

	/* clear flags except flag that the PF has set the MAC */
	adapter->vf_data[vf].flags &= IGB_VF_FLAG_PF_SET_MAC;
	adapter->vf_data[vf].last_nack = jiffies;

	/* reset offloads to defaults */
	igb_set_vmolr(adapter, vf, true);

	/* reset vlans for device */
	igb_clear_vf_vfta(adapter, vf);
#ifdef IFLA_VF_MAX
	if (adapter->vf_data[vf].pf_vlan)
		igb_ndo_set_vf_vlan(adapter->netdev, vf,
				    adapter->vf_data[vf].pf_vlan,
				    adapter->vf_data[vf].pf_qos);
	else
		igb_clear_vf_vfta(adapter, vf);
#endif

	/* reset multicast table array for vf */
	adapter->vf_data[vf].num_vf_mc_hashes = 0;

	/* Flush and reset the mta with the new values */
	igb_set_rx_mode(adapter->netdev);
	
	/*
	 * Reset the VFs TDWBAL and TDWBAH registers which are not
	 * cleared by a VFLR
	 */
	E1000_WRITE_REG(hw, E1000_TDWBAH(vf), 0);
	E1000_WRITE_REG(hw, E1000_TDWBAL(vf), 0);
	if (hw->mac.type == e1000_82576) {
		E1000_WRITE_REG(hw, E1000_TDWBAH(IGB_MAX_VF_FUNCTIONS + vf), 0);
		E1000_WRITE_REG(hw, E1000_TDWBAL(IGB_MAX_VF_FUNCTIONS + vf), 0);
	}
}

static void igb_vf_reset_event(struct igb_adapter *adapter, u32 vf)
{
	unsigned char *vf_mac = adapter->vf_data[vf].vf_mac_addresses;

	/* generate a new mac address as we were hotplug removed/added */
	if (!(adapter->vf_data[vf].flags & IGB_VF_FLAG_PF_SET_MAC))
		random_ether_addr(vf_mac);
    
	/* process remaining reset events */
	igb_vf_reset(adapter, vf);
}

static void igb_vf_reset_msg(struct igb_adapter *adapter, u32 vf)
{
	struct e1000_hw *hw = &adapter->hw;
	unsigned char *vf_mac = adapter->vf_data[vf].vf_mac_addresses;
	int rar_entry = hw->mac.rar_entry_count - (vf + 1);
	u32 reg, msgbuf[3];
	u8 *addr = (u8 *)(&msgbuf[1]);

	/* process all the same items cleared in a function level reset */
	igb_vf_reset(adapter, vf);

	/* set vf mac address */
	igb_rar_set_qsel(adapter, vf_mac, rar_entry, vf);

	/* enable transmit and receive for vf */
	reg = E1000_READ_REG(hw, E1000_VFTE);
	E1000_WRITE_REG(hw, E1000_VFTE, reg | (1 << vf));
	reg = E1000_READ_REG(hw, E1000_VFRE);
	E1000_WRITE_REG(hw, E1000_VFRE, reg | (1 << vf));

	adapter->vf_data[vf].flags |= IGB_VF_FLAG_CTS;

	/* reply to reset with ack and vf mac address */
	msgbuf[0] = E1000_VF_RESET | E1000_VT_MSGTYPE_ACK;
	memcpy(addr, vf_mac, 6);
	e1000_write_mbx(hw, msgbuf, 3, vf);
}

static int igb_set_vf_mac_addr(struct igb_adapter *adapter, u32 *msg, int vf)
{
	/*
	 * The VF MAC Address is stored in a packed array of bytes
	 * starting at the second 32 bit word of the msg array
	 */
	unsigned char *addr = (unsigned char *)&msg[1];
	int err = -1;

	if (is_valid_ether_addr(addr))
		err = igb_set_vf_mac(adapter, vf, addr);

	return err;
}

static void igb_rcv_ack_from_vf(struct igb_adapter *adapter, u32 vf)
{
	struct e1000_hw *hw = &adapter->hw;
	struct vf_data_storage *vf_data = &adapter->vf_data[vf];
	u32 msg = E1000_VT_MSGTYPE_NACK;

	/* if device isn't clear to send it shouldn't be reading either */
	if (!(vf_data->flags & IGB_VF_FLAG_CTS) &&
	    time_after(jiffies, vf_data->last_nack + (2 * HZ))) {
		e1000_write_mbx(hw, &msg, 1, vf);
		vf_data->last_nack = jiffies;
	}
}

static void igb_rcv_msg_from_vf(struct igb_adapter *adapter, u32 vf)
{
	u32 msgbuf[E1000_VFMAILBOX_SIZE];
	struct e1000_hw *hw = &adapter->hw;
	struct vf_data_storage *vf_data = &adapter->vf_data[vf];
	s32 retval;

	retval = e1000_read_mbx(hw, msgbuf, E1000_VFMAILBOX_SIZE, vf);

	if (retval) {
		pr_err( "Error receiving message from VF\n");
		return;
	}

	/* this is a message we already processed, do nothing */
	if (msgbuf[0] & (E1000_VT_MSGTYPE_ACK | E1000_VT_MSGTYPE_NACK))
		return;

	/*
	 * until the vf completes a reset it should not be
	 * allowed to start any configuration.
	 */

	if (msgbuf[0] == E1000_VF_RESET) {
		igb_vf_reset_msg(adapter, vf);
		return;
	}

	if (!(vf_data->flags & IGB_VF_FLAG_CTS)) {
		msgbuf[0] = E1000_VT_MSGTYPE_NACK;
		if (time_after(jiffies, vf_data->last_nack + (2 * HZ))) {
			e1000_write_mbx(hw, msgbuf, 1, vf);
			vf_data->last_nack = jiffies;
		}
		return;
	}

	switch ((msgbuf[0] & 0xFFFF)) {
	case E1000_VF_SET_MAC_ADDR:
		retval = -EINVAL;
#ifndef IGB_DISABLE_VF_MAC_SET
		if (!(vf_data->flags & IGB_VF_FLAG_PF_SET_MAC))
			retval = igb_set_vf_mac_addr(adapter, msgbuf, vf);
		else
			DPRINTK(DRV, INFO,
				"VF %d attempted to override administratively "
				"set MAC address\nReload the VF driver to "
				"resume operations\n", vf);
#endif
		break;
	case E1000_VF_SET_PROMISC:
		retval = igb_set_vf_promisc(adapter, msgbuf, vf);
		break;
	case E1000_VF_SET_MULTICAST:
		retval = igb_set_vf_multicasts(adapter, msgbuf, vf);
		break;
	case E1000_VF_SET_LPE:
		retval = igb_set_vf_rlpml(adapter, msgbuf[1], vf);
		break;
	case E1000_VF_SET_VLAN:
		retval = -1;
#ifdef IFLA_VF_MAX
		if (vf_data->pf_vlan)
			DPRINTK(DRV, INFO,
				"VF %d attempted to override administratively "
				"set VLAN tag\nReload the VF driver to "
				"resume operations\n", vf);
		else
#endif
			retval = igb_set_vf_vlan(adapter, msgbuf, vf);
		break;
	default:
		pr_err( "Unhandled Msg %08x\n", msgbuf[0]);
		retval = -E1000_ERR_MBX;
		break;
	}

	/* notify the VF of the results of what it sent us */
	if (retval)
		msgbuf[0] |= E1000_VT_MSGTYPE_NACK;
	else
		msgbuf[0] |= E1000_VT_MSGTYPE_ACK;

	msgbuf[0] |= E1000_VT_MSGTYPE_CTS;

	e1000_write_mbx(hw, msgbuf, 1, vf);
}

static void igb_msg_task(struct igb_adapter *adapter)
{
	struct e1000_hw *hw = &adapter->hw;
	u32 vf;

	for (vf = 0; vf < adapter->vfs_allocated_count; vf++) {
		/* process any reset requests */
		if (!e1000_check_for_rst(hw, vf))
			igb_vf_reset_event(adapter, vf);

		/* process any messages pending */
		if (!e1000_check_for_msg(hw, vf))
			igb_rcv_msg_from_vf(adapter, vf);

		/* process any acks */
		if (!e1000_check_for_ack(hw, vf))
			igb_rcv_ack_from_vf(adapter, vf);
	}
}

/**
 *  igb_set_uta - Set unicast filter table address
 *  @adapter: board private structure
 *
 *  The unicast table address is a register array of 32-bit registers.
 *  The table is meant to be used in a way similar to how the MTA is used
 *  however due to certain limitations in the hardware it is necessary to
 *  set all the hash bits to 1 and use the VMOLR ROPE bit as a promiscuous
 *  enable bit to allow vlan tag stripping when promiscuous mode is enabled
 **/
static void igb_set_uta(struct igb_adapter *adapter)
{
	struct e1000_hw *hw = &adapter->hw;
	int i;

	/* The UTA table only exists on 82576 hardware and newer */
	if (hw->mac.type < e1000_82576)
		return;

	/* we only need to do this if VMDq is enabled */
	if (!adapter->vmdq_pools)
		return;

	for (i = 0; i < hw->mac.uta_reg_count; i++)
		E1000_WRITE_REG_ARRAY(hw, E1000_UTA, i, ~0);
}

/**
 * igb_intr_msi - Interrupt Handler
 * @irq: interrupt number
 * @data: pointer to a network interface device structure
 **/
static irqreturn_t igb_intr_msi(int irq, void *data)
{
#ifndef __APPLE__
	struct igb_adapter *adapter = data;
	struct igb_q_vector *q_vector = adapter->q_vector[0];
	struct e1000_hw *hw = &adapter->hw;
	/* read ICR disables interrupts using IAM */
	u32 icr = E1000_READ_REG(hw, E1000_ICR);

	igb_write_itr(q_vector);

	if (icr & E1000_ICR_DRSTA)
		schedule_work(&adapter->reset_task);

	if (icr & E1000_ICR_DOUTSYNC) {
		/* HW is reporting DMA is out of sync */
		adapter->stats.doosync++;
	}

	if (icr & (E1000_ICR_RXSEQ | E1000_ICR_LSC)) {
		hw->mac.get_link_status = 1;
		if (!test_bit(__IGB_DOWN, &adapter->state))
			mod_timer(&adapter->watchdog_timer, jiffies + 1);
	}

#ifdef HAVE_PTP_1588_CLOCK
	if (icr & E1000_ICR_TS) {
		u32 tsicr = E1000_READ_REG(hw, E1000_TSICR);
		
		if (tsicr & E1000_TSICR_TXTS) {
			/* acknowledge the interrupt */
			E1000_WRITE_REG(hw, E1000_TSICR, E1000_TSICR_TXTS);
			/* retrieve hardware timestamp */
			schedule_work(&adapter->ptp_tx_work);
		}
	}
#endif /* HAVE_PTP_1588_CLOCK */

	napi_schedule(&q_vector->napi);
#endif
	return IRQ_HANDLED;
}

/**
 * igb_intr - Legacy Interrupt Handler
 * @irq: interrupt number
 * @data: pointer to a network interface device structure
 **/
static irqreturn_t igb_intr(int irq, void *data)
{
#ifndef __APPLE__
	struct igb_adapter *adapter = data;
	struct igb_q_vector *q_vector = adapter->q_vector[0];
	struct e1000_hw *hw = &adapter->hw;
	/* Interrupt Auto-Mask...upon reading ICR, interrupts are masked.  No
	 * need for the IMC write */
	u32 icr = E1000_READ_REG(hw, E1000_ICR);

	/* IMS will not auto-mask if INT_ASSERTED is not set, and if it is
	 * not set, then the adapter didn't send an interrupt */
	if (!(icr & E1000_ICR_INT_ASSERTED))
		return IRQ_NONE;

	igb_write_itr(q_vector);

	if (icr & E1000_ICR_DRSTA)
		schedule_work(&adapter->reset_task);

	if (icr & E1000_ICR_DOUTSYNC) {
		/* HW is reporting DMA is out of sync */
		adapter->stats.doosync++;
	}

	if (icr & (E1000_ICR_RXSEQ | E1000_ICR_LSC)) {
		hw->mac.get_link_status = 1;
		/* guard against interrupt when we're going down */
		if (!test_bit(__IGB_DOWN, &adapter->state))
			mod_timer(&adapter->watchdog_timer, jiffies + 1);
	}

	napi_schedule(&q_vector->napi);
#endif
	return IRQ_HANDLED;
}

void igb_ring_irq_enable(struct igb_q_vector *q_vector)
{
	struct igb_adapter *adapter = q_vector->adapter;
	struct e1000_hw *hw = &adapter->hw;

	if ((q_vector->rx.ring && (adapter->rx_itr_setting & 3)) ||
	    (!q_vector->rx.ring && (adapter->tx_itr_setting & 3))) {
		if ((adapter->num_q_vectors == 1) && !adapter->vf_data)
			igb_set_itr(q_vector);
		else
			igb_update_ring_itr(q_vector);
	}

	if (!test_bit(__IGB_DOWN, &adapter->state)) {
		if (adapter->msix_entries)
			E1000_WRITE_REG(hw, E1000_EIMS, q_vector->eims_value);
		else
			igb_irq_enable(adapter);
	}
}

/**
 * igb_poll - NAPI Rx polling callback
 * @napi: napi polling structure
 * @budget: count of how many packets we should handle
 **/
static int igb_poll(struct igb_q_vector *q_vector, int budget)
{
	bool clean_complete = true;

#ifdef IGB_DCA
	if (q_vector->adapter->flags & IGB_FLAG_DCA_ENABLED)
		igb_update_dca(q_vector);
#endif
	if (q_vector->tx.ring)
		clean_complete = igb_clean_tx_irq(q_vector);

	if (q_vector->rx.ring)
		clean_complete &= igb_clean_rx_irq(q_vector, budget);

#ifndef HAVE_NETDEV_NAPI_LIST
	/* if netdev is disabled we need to stop polling */
	if (!netif_running(q_vector->adapter->netdev))
		clean_complete = true;

#endif
#ifndef __APPLE__
	/* If all work not completed, return budget and keep polling */
	if (!clean_complete)
		return budget;

	/* If not enough Rx work done, exit the polling mode */
	napi_complete(napi);
#endif
	igb_ring_irq_enable(q_vector);
	return 0;
}

/**
 * igb_clean_tx_irq - Reclaim resources after transmit completes
 * @q_vector: pointer to q_vector containing needed info
 * returns TRUE if ring is completely cleaned
 **/
static bool igb_clean_tx_irq(struct igb_q_vector *q_vector)
{
	struct igb_adapter *adapter = q_vector->adapter;
	struct igb_ring *tx_ring = q_vector->tx.ring;
	struct igb_tx_buffer *tx_buffer;
	union e1000_adv_tx_desc *tx_desc;
	unsigned int total_bytes = 0, total_packets = 0;
	unsigned int budget = q_vector->tx.work_limit;
	unsigned int i = tx_ring->next_to_clean;

	if (test_bit(__IGB_DOWN, &adapter->state))
		return true;

	tx_buffer = &tx_ring->tx_buffer_info[i];
	tx_desc = IGB_TX_DESC(tx_ring, i);
	i -= tx_ring->count;

	do {
		union e1000_adv_tx_desc *eop_desc = tx_buffer->next_to_watch;

		/* prevent any other reads prior to eop_desc */
		rmb();

		/* if next_to_watch is not set then there is no work pending */
		if (!eop_desc)
			break;

		/* prevent any other reads prior to eop_desc */
		read_barrier_depends();

		/* if DD is not set pending work has not been completed */
		if (!(eop_desc->wb.status & cpu_to_le32(E1000_TXD_STAT_DD)))
			break;

		/* clear next_to_watch to prevent false hangs */
		tx_buffer->next_to_watch = NULL;

		/* update the statistics for this packet */
		total_bytes += tx_buffer->bytecount;
		total_packets += tx_buffer->gso_segs;

		/* free the skb */
		adapter->netdev->freePacket(tx_buffer->skb);

		/* unmap skb header data */
#ifndef	__APPLE__
		dma_unmap_single(tx_ring->dev,
		                 dma_unmap_addr(tx_buffer, dma),
		                 dma_unmap_len(tx_buffer, len),
		                 DMA_TO_DEVICE);
#endif
		/* clear tx_buffer data */
		tx_buffer->skb = NULL;
		dma_unmap_len_set(tx_buffer, len, 0);
		
		/* clear last DMA location and unmap remaining buffers */
		while (tx_desc != eop_desc) {
			tx_buffer++;
			tx_desc++;
			i++;
			if (unlikely(!i)) {
				i -= tx_ring->count;
				tx_buffer = tx_ring->tx_buffer_info;
				tx_desc = IGB_TX_DESC(tx_ring, 0);
			}

			/* unmap any remaining paged data */
#ifdef	__APPLE__
			dma_unmap_len_set(tx_buffer, len, 0);
#else
			if (dma_unmap_len(tx_buffer, len)) {
				dma_unmap_page(tx_ring->dev,
				               dma_unmap_addr(tx_buffer, dma),
				               dma_unmap_len(tx_buffer, len),
				               DMA_TO_DEVICE);
				dma_unmap_len_set(tx_buffer, len, 0);
			}
#endif
		}

		/* move us one more past the eop_desc for start of next pkt */
		tx_buffer++;
		tx_desc++;
		i++;
		if (unlikely(!i)) {
			i -= tx_ring->count;
			tx_buffer = tx_ring->tx_buffer_info;
			tx_desc = IGB_TX_DESC(tx_ring, 0);
		}

		/* issue prefetch for next Tx descriptor */
		prefetch(tx_desc);
		
		/* update budget accounting */
		budget--;
	} while (likely(budget));

#ifndef __APPLE__
	netdev_tx_completed_queue(txring_txq(tx_ring),
							  total_packets, total_bytes);
#endif
	i += tx_ring->count;
	tx_ring->next_to_clean = i;
	tx_ring->tx_stats.bytes += total_bytes;
	tx_ring->tx_stats.packets += total_packets;
	q_vector->tx.total_bytes += total_bytes;
	q_vector->tx.total_packets += total_packets;

#ifdef DEBUG
	if (test_bit(IGB_RING_FLAG_TX_DETECT_HANG, &tx_ring->flags) &&
	    !(adapter->disable_hw_reset && adapter->tx_hang_detected)) {
#else
	if (test_bit(IGB_RING_FLAG_TX_DETECT_HANG, &tx_ring->flags)) {
#endif
		struct e1000_hw *hw = &adapter->hw;

		/* Detect a transmit hang in hardware, this serializes the
		 * check with the clearing of time_stamp and movement of i */
		clear_bit(IGB_RING_FLAG_TX_DETECT_HANG, &tx_ring->flags);
		if (tx_buffer->next_to_watch &&
		    time_after(jiffies, tx_buffer->time_stamp +
		               (adapter->tx_timeout_factor * HZ))
		    && !(E1000_READ_REG(hw, E1000_STATUS) &
		         E1000_STATUS_TXOFF)) {

			/* detected Tx unit hang */
#ifdef DEBUG
			adapter->tx_hang_detected = TRUE;
			if (adapter->disable_hw_reset) {
				pr_err("Deactivating netdev watchdog timer\n");
#ifndef HAVE_NET_DEVICE_OPS
				//netdev_ring(tx_ring)->tx_timeout = NULL;
#endif
			}
#endif /* DEBUG */
			pr_err(
				"Detected Tx Unit Hang\n"
				"  Tx Queue             <%d>\n"
				"  TDH                  <%x>\n"
				"  TDT                  <%x>\n"
				"  next_to_use          <%x>\n"
				"  next_to_clean        <%x>\n"
				"buffer_info[next_to_clean]\n"
				"  time_stamp           <%lx>\n"
				"  next_to_watch        <%p>\n"
				"  desc.status          <%x>\n",
				tx_ring->queue_index,
				E1000_READ_REG(hw, E1000_TDH(tx_ring->reg_idx)),
				readl(tx_ring->tail),
				tx_ring->next_to_use,
				tx_ring->next_to_clean,
				tx_buffer->time_stamp,
				tx_buffer->next_to_watch,
				tx_buffer->next_to_watch->wb.status);
#ifdef	__APPLE__
				netif_stop_queue(netdev_ring(tx_ring));
#else
				if (netif_is_multiqueue(netdev_ring(tx_ring)))
				netif_stop_subqueue(netdev_ring(tx_ring),
						    ring_queue_index(tx_ring));
			else
				netif_stop_queue(netdev_ring(tx_ring));
#endif
			/* we are about to reset, no point in enabling stuff */
			return true;
		}
	}

#define TX_WAKE_THRESHOLD (DESC_NEEDED * 2)
	if (unlikely(total_packets &&
		     netif_carrier_ok(netdev_ring(tx_ring)) &&
		     igb_desc_unused(tx_ring) >= TX_WAKE_THRESHOLD)) {
		/* Make sure that anybody stopping the queue after this
		 * sees the new next_to_clean.
		 */
		smp_mb();
#ifdef	__APPLE__
		if (netif_queue_stopped(netdev_ring(tx_ring)) &&
			!(test_bit(__IGB_DOWN, &adapter->state))) {
			netif_wake_queue(netdev_ring(tx_ring));
			tx_ring->tx_stats.restart_queue++;
		}
#else
		if (netif_is_multiqueue(netdev_ring(tx_ring))) {
			if (__netif_subqueue_stopped(netdev_ring(tx_ring),
						     ring_queue_index(tx_ring)) &&
			    !(test_bit(__IGB_DOWN, &adapter->state))) {
				netif_wake_subqueue(netdev_ring(tx_ring),
						    ring_queue_index(tx_ring));
				tx_ring->tx_stats.restart_queue++;
			}
		} else {
			if (netif_queue_stopped(netdev_ring(tx_ring)) &&
			    !(test_bit(__IGB_DOWN, &adapter->state))) {
				netif_wake_queue(netdev_ring(tx_ring));
				tx_ring->tx_stats.restart_queue++;
			}
		}
#endif
	}

	return !!budget;
}

#ifdef HAVE_VLAN_RX_REGISTER
/**
 * igb_receive_skb - helper function to handle rx indications
 * @q_vector: structure containing interrupt and ring information
 * @skb: packet to send up
 **/
static void igb_receive_skb(struct igb_q_vector *q_vector,
                            struct sk_buff *skb)
{
#ifdef	__APPLE__
	q_vector->adapter->netdev->receive(skb);
#else
	struct vlan_group **vlgrp = netdev_priv(skb->dev);

	if (IGB_CB(skb)->vid) {
		if (*vlgrp) {
			vlan_gro_receive(&q_vector->napi, *vlgrp,
					 IGB_CB(skb)->vid, skb);
		} else {
			dev_kfree_skb_any(skb);
		}
	} else {
		napi_gro_receive(&q_vector->napi, skb);
	}
#endif
}

/**
 * igb_reuse_rx_page - page flip buffer and store it back on the ring
 * @rx_ring: rx descriptor ring to store buffers on
 * @old_buff: donor buffer to have page reused
 *
 * Synchronizes page for reuse by the adapter
 **/
static void igb_reuse_rx_page(struct igb_ring *rx_ring,
                              struct igb_rx_buffer *old_buff)
{
    struct igb_rx_buffer *new_buff;
    u16 nta = rx_ring->next_to_alloc;
    
    new_buff = &rx_ring->rx_buffer_info[nta];
    
    /* update, and store next to alloc */
    nta++;
    rx_ring->next_to_alloc = (nta < rx_ring->count) ? nta : 0;
    
    /* transfer page from old buffer to new buffer */
	*new_buff = *old_buff;
    
#ifndef	__APPLE__
    /* sync the buffer for use by the device */
    dma_sync_single_range_for_device(rx_ring->dev, old_buff->dma,
                                     old_buff->page_offset,
                                     IGB_RX_BUFSZ,
                                     DMA_FROM_DEVICE);
#endif
}

#ifndef	__APPLE__
static bool igb_can_reuse_rx_page(struct igb_rx_buffer *rx_buffer,
                                  struct page *page,
                                  unsigned int truesize)
{
    /* avoid re-using remote pages */
    if (unlikely(page_to_nid(page) != numa_node_id()))
        return false;
    
#if (PAGE_SIZE < 8192)
    /* if we are only owner of page we can reuse it */
    if (unlikely(page_count(page) != 1))
        return false;
    
    /* flip page offset to other buffer */
    rx_buffer->page_offset ^= IGB_RX_BUFSZ;
    
#else
    /* move offset up to the next cache line */
    rx_buffer->page_offset += truesize;
    
    if (rx_buffer->page_offset > (PAGE_SIZE - IGB_RX_BUFSZ))
        return false;
#endif
    
    /* bump ref count on page before it is given to the stack */
    get_page(page);
    return true;
}
#endif //__APPLE__

/**
 * igb_add_rx_frag - Add contents of Rx buffer to sk_buff
 * @rx_ring: rx descriptor ring to transact packets on
 * @rx_buffer: buffer containing page to add
 * @rx_desc: descriptor containing length of buffer written by hardware
 * @skb: sk_buff to place the data into
 *
 * This function will add the data contained in rx_buffer->page to the skb.
 * This is done either through a direct copy if the data in the buffer is
 * less than the skb header size, otherwise it will just attach the page as
 * a frag to the skb.
 *
 * The function will then update the page offset if necessary and return
 * true if the buffer can be reused by the adapter.
 **/
static bool igb_add_rx_frag(struct igb_ring *rx_ring,
                            struct igb_rx_buffer *rx_buffer,
                            union e1000_adv_rx_desc *rx_desc,
                            struct sk_buff *skb)
{
#ifdef __APPLE__
    IOBufferMemoryDescriptor *page = rx_buffer->page;
    unsigned int size = le16_to_cpu(rx_desc->wb.upper.length);

    unsigned char *va = (u8*)page->getBytesNoCopy() + rx_buffer->page_offset;
        
#ifdef HAVE_PTP_1588_CLOCK
        if (igb_test_staterr(rx_desc, E1000_RXDADV_STAT_TSIP)) {
            igb_ptp_rx_pktstamp(rx_ring->q_vector, va, skb);
            va += IGB_TS_HDR_LEN;
            size -= IGB_TS_HDR_LEN;
        }
#endif /* HAVE_PTP_1588_CLOCK */
        
    mbuf_copyback(skb, mbuf_pkthdr_len(skb), size,
                      va,
                      MBUF_WAITOK);
    return true;
#else //__APPLE__
    struct page *page = rx_buffer->page;
	unsigned char *va = page_address(page) + rx_buffer->page_offset;
    unsigned int size = le16_to_cpu(rx_desc->wb.upper.length);

#if (PAGE_SIZE < 8192)
    unsigned int truesize = IGB_RX_BUFSZ;
#else
    unsigned int truesize = SKB_DATA_ALIGN(size, L1_CACHE_BYTES);
#endif
	unsigned int pull_len;

	if (unlikely(skb_is_nonlinear(skb)))
		goto add_tail_frag;
        
#ifdef HAVE_PTP_1588_CLOCK
	if (unlikely(igb_test_staterr(rx_desc, E1000_RXDADV_STAT_TSIP))) {
		igb_ptp_rx_pktstamp(rx_ring->q_vector, va, skb);
		va += IGB_TS_HDR_LEN;
		size -= IGB_TS_HDR_LEN;
	}
#endif /* HAVE_PTP_1588_CLOCK */
        
	if (likely(size <= IGB_RX_HDR_LEN)) {
        memcpy(__skb_put(skb, size), va, ALIGN(size, sizeof(long)));
        
        /* we can reuse buffer as-is, just make sure it is local */
        if (likely(page_to_nid(page) == numa_node_id()))
            return true;
        
        /* this page cannot be reused so discard it */
        put_page(page);
        return false;
    }
    
	/* we need the header to contain the greater of either ETH_HLEN or
	 * 60 bytes if the skb->len is less than 60 for skb_pad.
	 */
	pull_len = eth_get_headlen(va, IGB_RX_HDR_LEN);
	
	/* align pull length to size of long to optimize memcpy performance */
	memcpy(__skb_put(skb, pull_len), va, ALIGN(pull_len, sizeof(long)));
	
	/* update all of the pointers */
	va += pull_len;
	size -= pull_len;
	
add_tail_frag:
    skb_add_rx_frag(skb, skb_shinfo(skb)->nr_frags, page,
                    (unsigned long)va & ~PAGE_MASK, size, truesize);

    return igb_can_reuse_rx_page(rx_buffer, page, truesize);
#endif // __APPLE__
}

static struct sk_buff *igb_fetch_rx_buffer(struct igb_ring *rx_ring,
                                           union e1000_adv_rx_desc *rx_desc,
                                           struct sk_buff *skb)
{
    struct igb_rx_buffer *rx_buffer;
#ifdef	__APPLE__
    IOBufferMemoryDescriptor *page;
#else
    struct page *page;
#endif
    
    rx_buffer = &rx_ring->rx_buffer_info[rx_ring->next_to_clean];

    page = rx_buffer->page;
    prefetchw(page);

    if (likely(!skb)) {
#ifndef	__APPLE__
        void *page_addr = page_address(page) +
        rx_buffer->page_offset;
#endif
        /* prefetch first cache line of first page */
        prefetch(page_addr);
#if L1_CACHE_BYTES < 128
        prefetch(page_addr + L1_CACHE_BYTES);
#endif
        
        /* allocate a skb to store the frags */
        skb = netdev_alloc_skb_ip_align(rx_ring->netdev,
                                        IGB_RX_HDR_LEN);
        if (unlikely(!skb)) {
            rx_ring->rx_stats.alloc_failed++;
            return NULL;
        }
        /*
         * we will be copying header into skb->data in
         * pskb_may_pull so it is in our interest to prefetch
         * it now to avoid a possible cache miss
         */
        prefetchw(skb->data);
    }
    
    /* we are reusing so sync this buffer for CPU use */
#ifdef	__APPLE__
#else
    dma_sync_single_range_for_cpu(rx_ring->dev,
                                  rx_buffer->dma,
                                  rx_buffer->page_offset,
                                  IGB_RX_BUFSZ,
                                  DMA_FROM_DEVICE);
#endif

    /* pull page into skb */
#ifdef	__APPLE__
    igb_add_rx_frag(rx_ring, rx_buffer, rx_desc, skb);
    igb_reuse_rx_page(rx_ring, rx_buffer);
#else
    if (igb_add_rx_frag(rx_ring, rx_buffer, rx_desc, skb)) {
        /* hand second half of page back to the ring */
        igb_reuse_rx_page(rx_ring, rx_buffer);
    } else {
        /* we are not reusing the buffer so unmap it */
        dma_unmap_page(rx_ring->dev, rx_buffer->dma,
                       PAGE_SIZE, DMA_FROM_DEVICE);
    }
    
#endif
    /* clear contents of rx_buffer */
    rx_buffer->page = NULL;

    return skb;
}
	

#endif /* HAVE_VLAN_RX_REGISTER */
static inline void igb_rx_checksum(struct igb_ring *ring,
				   union e1000_adv_rx_desc *rx_desc,
				   struct sk_buff *skb)
{
	//skb_checksum_none_assert(skb);

	/* Ignore Checksum bit is set */
	if (igb_test_staterr(rx_desc, E1000_RXD_STAT_IXSM))
		return;

	/* Rx checksum disabled via ethtool */
	if (!(ring->netdev->features() & NETIF_F_RXCSUM))
		return;

	/* TCP/UDP checksum error bit is set */
	if (igb_test_staterr(rx_desc,
				E1000_RXDEXT_STATERR_TCPE |
			    E1000_RXDEXT_STATERR_IPE)) {
		/*
		 * work around errata with sctp packets where the TCPE aka
		 * L4E bit is set incorrectly on 64 byte (60 byte w/o crc)
		 * packets, (aka let the stack check the crc32c)
		 */
		if (!((mbuf_pkthdr_len(skb) == 60) &&
		      test_bit(IGB_RING_FLAG_RX_SCTP_CSUM, &ring->flags)))
			ring->rx_stats.csum_err++;

		/* let the stack verify checksum errors */
		return;
	}
	/* It must be a TCP or UDP packet with a valid checksum */
#ifdef	__APPLE__
    UInt32 flag = 0;
	if (igb_test_staterr(rx_desc, E1000_RXD_STAT_IPCS)){
        flag |= IONetworkController::kChecksumIP;
    }
	if (igb_test_staterr(rx_desc, (E1000_RXD_STAT_TCPCS))){
        flag |= IONetworkController::kChecksumTCP | CSUM_TCPIPv6;
    }
	if (igb_test_staterr(rx_desc, (E1000_RXD_STAT_UDPCS))){
        flag |= IONetworkController::kChecksumUDP | CSUM_UDPIPv6;
    }
    if(flag)
        ring->netdev->rxChecksumOK(skb, flag);
#else
	if (igb_test_staterr(rx_desc, E1000_RXD_STAT_TCPCS |
						 E1000_RXD_STAT_UDPCS))
		skb->ip_summed = CHECKSUM_UNNECESSARY;
#endif
}

#ifdef NETIF_F_RXHASH
static inline void igb_rx_hash(struct igb_ring *ring,
			       union e1000_adv_rx_desc *rx_desc,
			       struct sk_buff *skb)
{
	if (((AppleIGB*)netdev_ring(ring))->features() & NETIF_F_RXHASH)
		skb_set_hash(skb,
					 le32_to_cpu(rx_desc->wb.lower.hi_dword.rss),
					 PKT_HASH_TYPE_L3);
}

#endif

/**
 * igb_process_skb_fields - Populate skb header fields from Rx descriptor
 * @rx_ring: rx descriptor ring packet is being transacted on
 * @rx_desc: pointer to the EOP Rx descriptor
 * @skb: pointer to current skb being populated
 *
 * This function checks the ring, descriptor, and packet information in
 * order to populate the hash, checksum, VLAN, timestamp, protocol, and
 * other fields within the skb.
 **/
static void igb_process_skb_fields(struct igb_ring *rx_ring,
								   union e1000_adv_rx_desc *rx_desc,
								   struct sk_buff *skb)
{
#ifdef __APPLE__
	u16 vid = 0;
	AppleIGB *dev = rx_ring->netdev;

#ifdef NETIF_F_RXHASH
	igb_rx_hash(rx_ring, rx_desc, skb);
	
#endif
	igb_rx_checksum(rx_ring, rx_desc, skb);
	
	if (igb_test_staterr(rx_desc, E1000_RXD_STAT_VP)) {
		if (igb_test_staterr(rx_desc, E1000_RXDEXT_STATERR_LB) &&
		    test_bit(IGB_RING_FLAG_RX_LB_VLAN_BSWAP, &rx_ring->flags))
			vid = be16_to_cpu(rx_desc->wb.upper.vlan);
		else
			vid = le16_to_cpu(rx_desc->wb.upper.vlan);
	}
	if(vid){
        dev->setVid(skb,(UInt32)vid);
    }

#else // __APPLE__
	struct net_device *dev = rx_ring->netdev;
	__le16 pkt_info = rx_desc->wb.lower.lo_dword.hs_rss.pkt_info;

#ifdef NETIF_F_RXHASH
	igb_rx_hash(rx_ring, rx_desc, skb);
	
#endif
	igb_rx_checksum(rx_ring, rx_desc, skb);
	
    /* update packet type stats */
	switch (pkt_info & E1000_RXDADV_PKTTYPE_ILMASK) {
	case E1000_RXDADV_PKTTYPE_IPV4:
		rx_ring->pkt_stats.ipv4_packets++;
		break;
	case E1000_RXDADV_PKTTYPE_IPV4_EX:
		rx_ring->pkt_stats.ipv4e_packets++;
		break;
	case E1000_RXDADV_PKTTYPE_IPV6:
		rx_ring->pkt_stats.ipv6_packets++;
		break;
	case E1000_RXDADV_PKTTYPE_IPV6_EX:
		rx_ring->pkt_stats.ipv6e_packets++;
		break;
	default:
		notype = true;
		break;
	}
	
	switch (pkt_info & E1000_RXDADV_PKTTYPE_TLMASK) {
	case E1000_RXDADV_PKTTYPE_TCP:
		rx_ring->pkt_stats.tcp_packets++;
		break;
	case E1000_RXDADV_PKTTYPE_UDP:
		rx_ring->pkt_stats.udp_packets++;
		break;
	case E1000_RXDADV_PKTTYPE_SCTP:
		rx_ring->pkt_stats.sctp_packets++;
		break;
	case E1000_RXDADV_PKTTYPE_NFS:
		rx_ring->pkt_stats.nfs_packets++;
		break;
	case E1000_RXDADV_PKTTYPE_NONE:
		if (notype)
			rx_ring->pkt_stats.other_packets++;
		break;
	default:
		break;
	}

#ifdef HAVE_PTP_1588_CLOCK
	if (igb_test_staterr(rx_desc, E1000_RXDADV_STAT_TS) &&
	    !igb_test_staterr(rx_desc, E1000_RXDADV_STAT_TSIP))
		igb_ptp_rx_rgtstamp(rx_ring->q_vector, skb);
#endif /* HAVE_PTP_1588_CLOCK */
	
#ifdef NETIF_F_HW_VLAN_CTAG_RX
	if ((dev->features & NETIF_F_HW_VLAN_CTAG_RX) &&
#else
	if ((dev->features & NETIF_F_HW_VLAN_RX) &&
#endif
	    igb_test_staterr(rx_desc, E1000_RXD_STAT_VP)) {
		u16 vid = 0;
		if (igb_test_staterr(rx_desc, E1000_RXDEXT_STATERR_LB) &&
		    test_bit(IGB_RING_FLAG_RX_LB_VLAN_BSWAP, &rx_ring->flags))
			vid = be16_to_cpu(rx_desc->wb.upper.vlan);
		else
			vid = le16_to_cpu(rx_desc->wb.upper.vlan);
#ifdef HAVE_VLAN_RX_REGISTER
		IGB_CB(skb)->vid = vid;
	} else {
		IGB_CB(skb)->vid = 0;
#else
		__vlan_hwaccel_put_tag(skb, htons(ETH_P_8021Q), vid);
#endif
	}
	
	skb_record_rx_queue(skb, rx_ring->queue_index);
	
	skb->protocol = eth_type_trans(skb, dev);
#endif // __APPLE__
}

/**
 * igb_is_non_eop - process handling of non-EOP buffers
 * @rx_ring: Rx ring being processed
 * @rx_desc: Rx descriptor for current buffer
 *
 * This function updates next to clean.  If the buffer is an EOP buffer
 * this function exits returning false, otherwise it will place the
 * sk_buff in the next buffer to be chained and return true indicating
 * that this is in fact a non-EOP buffer.
 **/
static bool igb_is_non_eop(struct igb_ring *rx_ring,
						   union e1000_adv_rx_desc *rx_desc)
{
	u32 ntc = rx_ring->next_to_clean + 1;
	
	/* fetch, update, and store next to clean */
	ntc = (ntc < rx_ring->count) ? ntc : 0;
	rx_ring->next_to_clean = ntc;
	
	prefetch(IGB_RX_DESC(rx_ring, ntc));
	
	if (likely(igb_test_staterr(rx_desc, E1000_RXD_STAT_EOP)))
		return false;
	
	return true;
}

/**
 * igb_pull_tail - igb specific version of skb_pull_tail
 * @rx_ring: rx descriptor ring packet is being transacted on
 * @rx_desc: pointer to the EOP Rx descriptor
 * @skb: pointer to current skb being adjusted
 *
 * This function is an igb specific version of __pskb_pull_tail.  The
 * main difference between this version and the original function is that
 * this function can make several assumptions about the state of things
 * that allow for significant optimizations versus the standard function.
 * As a result we can do things like drop a frag and maintain an accurate
 * truesize for the skb.
 */
static void igb_pull_tail(struct igb_ring *rx_ring,
						  union e1000_adv_rx_desc *rx_desc,
						  struct sk_buff *skb)
{
#ifdef __APPLE__
#else
	struct skb_frag_struct *frag = &skb_shinfo(skb)->frags[0];
	unsigned char *va;
	unsigned int pull_len;
	
	/*
	 * it is valid to use page_address instead of kmap since we are
	 * working with pages allocated out of the lomem pool per
	 * alloc_page(GFP_ATOMIC)
	 */
	va = skb_frag_address(frag);
	
#ifdef HAVE_PTP_1588_CLOCK
	if (igb_test_staterr(rx_desc, E1000_RXDADV_STAT_TSIP)) {
		/* retrieve timestamp from buffer */
		igb_ptp_rx_pktstamp(rx_ring->q_vector, va, skb);
		
		/* update pointers to remove timestamp header */
		skb_frag_size_sub(frag, IGB_TS_HDR_LEN);
		frag->page_offset += IGB_TS_HDR_LEN;
		skb->data_len -= IGB_TS_HDR_LEN;
		skb->len -= IGB_TS_HDR_LEN;
		
		/* move va to start of packet data */
		va += IGB_TS_HDR_LEN;
	}
#endif /* HAVE_PTP_1588_CLOCK */
	
	/*
	 * we need the header to contain the greater of either ETH_HLEN or
	 * 60 bytes if the skb->len is less than 60 for skb_pad.
	 */
	pull_len = igb_get_headlen(va, IGB_RX_HDR_LEN);
	
	/* align pull length to size of long to optimize memcpy performance */
	skb_copy_to_linear_data(skb, va, ALIGN(pull_len, sizeof(long)));
	
	/* update all of the pointers */
	skb_frag_size_sub(frag, pull_len);
	frag->page_offset += pull_len;
	skb->data_len -= pull_len;
	skb->tail += pull_len;
#endif
}

/**
 * igb_cleanup_headers - Correct corrupted or empty headers
 * @rx_ring: rx descriptor ring packet is being transacted on
 * @rx_desc: pointer to the EOP Rx descriptor
 * @skb: pointer to current skb being fixed
 *
 * Address the case where we are pulling data in on pages only
 * and as such no data is present in the skb header.
 *
 * In addition if skb is not at least 60 bytes we need to pad it so that
 * it is large enough to qualify as a valid Ethernet frame.
 *
 * Returns true if an error was encountered and skb was freed.
 **/
static bool igb_cleanup_headers(struct igb_ring *rx_ring,
								union e1000_adv_rx_desc *rx_desc,
								struct sk_buff *skb)
{
#ifdef __APPLE__
	if (unlikely((igb_test_staterr(rx_desc,
								   E1000_RXDEXT_ERR_FRAME_ERR_MASK)))) {
        AppleIGB* netdev = (AppleIGB*)rx_ring->netdev;
        netdev->getNetStats()->inputErrors += 1;
		netdev->freePacket(skb);
		return true;
	}
#else
	if (unlikely((igb_test_staterr(rx_desc,
								   E1000_RXDEXT_ERR_FRAME_ERR_MASK)))) {
		struct net_device *netdev = rx_ring->netdev;

		if (!(netdev->features & NETIF_F_RXALL)) {
			dev_kfree_skb_any(skb);
			return true;
		}
	}
	
	/* if skb_pad returns an error the skb was freed */
	if (unlikely(skb->len < 60)) {
		int pad_len = 60 - skb->len;
		
		if (skb_pad(skb, pad_len))
			return true;
		__skb_put(skb, pad_len);
	}
#endif
	return false;
}

/* igb_clean_rx_irq -- * packet split */
static bool igb_clean_rx_irq(struct igb_q_vector *q_vector, int budget)
{
	struct igb_ring *rx_ring = q_vector->rx.ring;
	struct sk_buff *skb = rx_ring->skb;
	unsigned int total_bytes = 0, total_packets = 0;
	u16 cleaned_count = igb_desc_unused(rx_ring);

	do {
		union e1000_adv_rx_desc *rx_desc;

		/* return some buffers to hardware, one at a time is too slow */
		if (cleaned_count >= IGB_RX_BUFFER_WRITE) {
			igb_alloc_rx_buffers(rx_ring, cleaned_count);
			cleaned_count = 0;
		}

		rx_desc = IGB_RX_DESC(rx_ring, rx_ring->next_to_clean);

		if (!igb_test_staterr(rx_desc, E1000_RXD_STAT_DD))
			break;
		
		/*
		 * This memory barrier is needed to keep us from reading
		 * any other fields out of the rx_desc until we know the
		 * RXD_STAT_DD bit is set
		 */
		rmb();
		
		/* retrieve a buffer from the ring */
		skb = igb_fetch_rx_buffer(rx_ring, rx_desc, skb);
		
		/* exit if we failed to retrieve a buffer */
		if (!skb)
			break;
        
		cleaned_count++;
		
		/* fetch next buffer in frame if non-eop */
		if (igb_is_non_eop(rx_ring, rx_desc))
			continue;
		
		/* verify the packet layout is correct */
		if (igb_cleanup_headers(rx_ring, rx_desc, skb)) {
			skb = NULL;
			continue;
		}

		/* probably a little skewed due to removing CRC */
#ifdef __APPLE__
		total_bytes += mbuf_pkthdr_len(skb);
#else
		total_bytes += skb->len;
#endif

		/* populate checksum, timestamp, VLAN, and protocol */
		igb_process_skb_fields(rx_ring, rx_desc, skb);
		
#ifndef IGB_NO_LRO
		if (igb_can_lro(rx_ring, rx_desc, skb))
			igb_lro_receive(q_vector, skb);
		else
#endif
#ifdef HAVE_VLAN_RX_REGISTER
			igb_receive_skb(q_vector, skb);
#else
		napi_gro_receive(&q_vector->napi, skb);
#endif
#ifndef NETIF_F_GRO
		
		netdev_ring(rx_ring)->last_rx = jiffies;
#endif
		
		/* reset skb pointer */
		skb = NULL;
		
		/* update budget accounting */
		total_packets++;
	} while (likely(total_packets < budget));

	/* place incomplete frames back on ring for completion */
	rx_ring->skb = skb;
	
	rx_ring->rx_stats.packets += total_packets;
	rx_ring->rx_stats.bytes += total_bytes;
	q_vector->rx.total_packets += total_packets;
	q_vector->rx.total_bytes += total_bytes;
	
	if (cleaned_count)
		igb_alloc_rx_buffers(rx_ring, cleaned_count);
	
#ifndef IGB_NO_LRO
	igb_lro_flush_all(q_vector);
	
#endif /* IGB_NO_LRO */
	return (total_packets < budget);
}

	
	
static bool igb_alloc_mapped_page(struct igb_ring *rx_ring,
								  struct igb_rx_buffer *bi)
{
#ifdef __APPLE__
	dma_addr_t dma;

	/* since we are recycling buffers we should seldom need to alloc */
	if (likely(bi->page))
		return true;

	/* alloc new page for storage */
	bi->page = 	IOBufferMemoryDescriptor::inTaskWithOptions( kernel_task,
                            kIODirectionInOut | kIOMemoryPhysicallyContiguous,
                            PAGE_SIZE, PAGE_SIZE );

	if (unlikely(!bi->page)) {
		rx_ring->rx_stats.alloc_failed++;
		return false;
	}

	/* map page for use */
    bi->page->prepare();
    dma = bi->page->getPhysicalAddress();

	bi->dma = dma;
#else
	struct page *page = bi->page;
	dma_addr_t dma;
	
	/* since we are recycling buffers we should seldom need to alloc */
	if (likely(page))
		return true;
	
	/* alloc new page for storage */
	page = alloc_page(GFP_ATOMIC | __GFP_COLD);
	if (unlikely(!page)) {
		rx_ring->rx_stats.alloc_failed++;
		return false;
	}
	
	/* map page for use */
	dma = dma_map_page(rx_ring->dev, page, 0, PAGE_SIZE, DMA_FROM_DEVICE);
	
	/*
	 * if mapping failed free memory back to system since
	 * there isn't much point in holding memory we can't use
	 */
	if (dma_mapping_error(rx_ring->dev, dma)) {
		__free_page(page);
		
		rx_ring->rx_stats.alloc_failed++;
		return false;
	}
	
	bi->dma = dma;
	bi->page = page;
#endif
	bi->page_offset = 0;
	return true;
}
	



/**
 * igb_alloc_rx_buffers - Replace used receive buffers; packet split
 * @adapter: address of board private structure
 **/
void igb_alloc_rx_buffers(struct igb_ring *rx_ring, u16 cleaned_count)
{
	union e1000_adv_rx_desc *rx_desc;
	struct igb_rx_buffer *bi;
	u16 i = rx_ring->next_to_use;

	rx_desc = IGB_RX_DESC(rx_ring, i);
	bi = &rx_ring->rx_buffer_info[i];
	i -= rx_ring->count;

	/* nothing to do */
	if (!cleaned_count)
		return;

	do {
		if (!igb_alloc_mapped_page(rx_ring, bi))
			break;

		/*
		 * Refresh the desc even if buffer_addrs didn't change
		 * because each write-back erases this info.
		 */
		rx_desc->read.pkt_addr = cpu_to_le64(bi->dma + bi->page_offset);
		rx_desc++;
		bi++;
		i++;
		if (unlikely(!i)) {
			rx_desc = IGB_RX_DESC(rx_ring, 0);
			bi = rx_ring->rx_buffer_info;
			i -= rx_ring->count;
		}

		/* clear the hdr_addr for the next_to_use descriptor */
		rx_desc->read.hdr_addr = 0;

		cleaned_count--;
	} while (cleaned_count);

	i += rx_ring->count;

	if (rx_ring->next_to_use != i) {
		/* record the next descriptor to use */
		rx_ring->next_to_use = i;

		/* update next to alloc since we have filled the ring */
		rx_ring->next_to_alloc = i;

		/*
		 * Force memory writes to complete before letting h/w
		 * know there are new descriptors to fetch.  (Only
		 * applicable for weak-ordered memory model archs,
		 * such as IA-64).
		 */
		wmb();
		writel(i, rx_ring->tail);
	}
}

#ifdef SIOCGMIIPHY
/**
 * igb_mii_ioctl -
 * @netdev:
 * @ifreq:
 * @cmd:
 **/
static int igb_mii_ioctl(IOEthernetController *netdev, struct ifreq *ifr, int cmd)
{
	struct igb_adapter *adapter = netdev_priv(netdev);
	struct mii_ioctl_data *data = if_mii(ifr);

	if (adapter->hw.phy.media_type != e1000_media_type_copper)
		return -EOPNOTSUPP;

	switch (cmd) {
	case SIOCGMIIPHY:
		data->phy_id = adapter->hw.phy.addr;
		break;
	case SIOCGMIIREG:
		if (!capable(CAP_NET_ADMIN))
			return -EPERM;
		if (e1000_read_phy_reg(&adapter->hw, data->reg_num & 0x1F,
				   &data->val_out))
			return -EIO;
		break;
	case SIOCSMIIREG:
	default:
		return -EOPNOTSUPP;
	}
	return E1000_SUCCESS;
}

#endif

#ifndef	__APPLE__
/**
 * igb_ioctl -
 * @netdev:
 * @ifreq:
 * @cmd:
 **/
static int igb_ioctl(IOEthernetController *netdev, struct ifreq *ifr, int cmd)
{
	switch (cmd) {
#ifdef SIOCGMIIPHY
	case SIOCGMIIPHY:
	case SIOCGMIIREG:
	case SIOCSMIIREG:
		return igb_mii_ioctl(netdev, ifr, cmd);
#endif
#ifdef HAVE_PTP_1588_CLOCK
#ifdef SIOCGHWTSTAMP
		case SIOCGHWTSTAMP:
			return igb_ptp_get_ts_config(netdev, ifr);
#endif
		case SIOCSHWTSTAMP:
			return igb_ptp_set_ts_config(netdev, ifr);
#endif /* HAVE_PTP_1588_CLOCK */
#ifdef ETHTOOL_OPS_COMPAT
	case SIOCETHTOOL:
		return ethtool_ioctl(ifr);
#endif
	default:
		return -EOPNOTSUPP;
	}
}
#endif

void e1000_read_pci_cfg(struct e1000_hw *hw, u32 reg, u16 *value)
{
	struct igb_adapter *adapter = (igb_adapter *)hw->back;
	
	*value = adapter->pdev->configRead16(reg);
}
		
void e1000_write_pci_cfg(struct e1000_hw *hw, u32 reg, u16 *value)
{
	struct igb_adapter *adapter = (igb_adapter*)hw->back;

	adapter->pdev->configWrite16(reg,*value);
}

s32 e1000_read_pcie_cap_reg(struct e1000_hw *hw, u32 reg, u16 *value)
{
	struct igb_adapter *adapter = (igb_adapter*)hw->back;
	u8 cap_offset;

	if (0 == adapter->pdev->findPCICapability(kIOPCIPCIExpressCapability, &cap_offset))
		return -E1000_ERR_CONFIG;

	*value = adapter->pdev->configRead16(cap_offset + reg);

	return E1000_SUCCESS;
}

s32 e1000_write_pcie_cap_reg(struct e1000_hw *hw, u32 reg, u16 *value)
{
	struct igb_adapter *adapter = (igb_adapter*)hw->back;
	u8 cap_offset;

	if (0 == adapter->pdev->findPCICapability(kIOPCIPCIExpressCapability, &cap_offset))
		return -E1000_ERR_CONFIG;
	
	adapter->pdev->configWrite16(cap_offset + reg, *value);

	return E1000_SUCCESS;
}

#ifdef HAVE_VLAN_RX_REGISTER
static void igb_vlan_mode(IOEthernetController *netdev, struct vlan_group *vlgrp)
#else
void igb_vlan_mode(IOEthernetController *netdev, u32 features)
#endif /* HAVE_VLAN_RX_REGISTER */
{
	struct igb_adapter *adapter = netdev_priv(netdev);
	struct e1000_hw *hw = &adapter->hw;
	bool enable;
	u32 ctrl, rctl;
#ifdef __APPLE__
	enable = TRUE;
	if (enable) {
		/* enable VLAN tag insert/strip */
		ctrl = E1000_READ_REG(hw, E1000_CTRL);
		ctrl |= E1000_CTRL_VME;
		E1000_WRITE_REG(hw, E1000_CTRL, ctrl);
        
		/* Disable CFI check */
		rctl = E1000_READ_REG(hw, E1000_RCTL);
		rctl &= ~E1000_RCTL_CFIEN;
		E1000_WRITE_REG(hw, E1000_RCTL, rctl);
	} else {
		/* disable VLAN tag insert/strip */
		ctrl = E1000_READ_REG(hw, E1000_CTRL);
		ctrl &= ~E1000_CTRL_VME;
		E1000_WRITE_REG(hw, E1000_CTRL, ctrl);
	}
#else //__APPLE__
    
    int i;
#ifdef HAVE_VLAN_RX_REGISTER
	enable = !!vlgrp;

	igb_irq_disable(adapter);

	adapter->vlgrp = vlgrp;

	if (!test_bit(__IGB_DOWN, &adapter->state))
		igb_irq_enable(adapter);
#else
#ifdef NETIF_F_HW_VLAN_CTAG_RX
	bool enable = !!(features & NETIF_F_HW_VLAN_CTAG_RX);
#else
	bool enable = !!(features & NETIF_F_HW_VLAN_RX);
#endif /* NETIF_F_HW_VLAN_CTAG_RX */
#endif /* HAVE_VLAN_RX_REGISTER */

	if (enable) {
		/* enable VLAN tag insert/strip */
		ctrl = E1000_READ_REG(hw, E1000_CTRL);
		ctrl |= E1000_CTRL_VME;
		E1000_WRITE_REG(hw, E1000_CTRL, ctrl);

		/* Disable CFI check */
		rctl = E1000_READ_REG(hw, E1000_RCTL);
		rctl &= ~E1000_RCTL_CFIEN;
		E1000_WRITE_REG(hw, E1000_RCTL, rctl);
	} else {
		/* disable VLAN tag insert/strip */
		ctrl = E1000_READ_REG(hw, E1000_CTRL);
		ctrl &= ~E1000_CTRL_VME;
		E1000_WRITE_REG(hw, E1000_CTRL, ctrl);
	}

	for (i = 0; i < adapter->vmdq_pools; i++) {
		igb_set_vf_vlan_strip(adapter,
				      adapter->vfs_allocated_count + i,
				      enable);
	}

#endif //__APPLE__
	igb_rlpml_set(adapter);
}

#ifdef HAVE_INT_NDO_VLAN_RX_ADD_VID
#ifdef NETIF_F_HW_VLAN_CTAG_RX
static int igb_vlan_rx_add_vid(struct net_device *netdev,
									   __always_unused __be16 proto, u16 vid)
#else
static int igb_vlan_rx_add_vid(IOEthernetController *netdev, u16 vid)
#endif
#else
static void igb_vlan_rx_add_vid(IOEthernetController *netdev, u16 vid)
#endif
{
	struct igb_adapter *adapter = netdev_priv(netdev);
	int pf_id = adapter->vfs_allocated_count;

	/* attempt to add filter to vlvf array */
	igb_vlvf_set(adapter, vid, TRUE, pf_id);

	/* add the filter since PF can receive vlans w/o entry in vlvf */
	igb_vfta_set(adapter, vid, TRUE);

#ifndef HAVE_VLAN_RX_REGISTER

	set_bit(vid, adapter->active_vlans);
#endif
#ifdef HAVE_INT_NDO_VLAN_RX_ADD_VID
		return 0;
#endif
}

#ifndef __APPLE__
#ifdef HAVE_INT_NDO_VLAN_RX_ADD_VID
#ifdef NETIF_F_HW_VLAN_CTAG_RX
		static int igb_vlan_rx_kill_vid(struct net_device *netdev,
										__always_unused __be16 proto, u16 vid)
#else
		static int igb_vlan_rx_kill_vid(struct net_device *netdev, u16 vid)
#endif
#else
		static void igb_vlan_rx_kill_vid(struct net_device *netdev, u16 vid)
#endif
	{
		struct igb_adapter *adapter = netdev_priv(netdev);
		int pf_id = adapter->vfs_allocated_count;
		s32 err;
		
#ifdef HAVE_VLAN_RX_REGISTER
		igb_irq_disable(adapter);
		
		vlan_group_set_device(adapter->vlgrp, vid, NULL);
		
		if (!test_bit(__IGB_DOWN, &adapter->state))
			igb_irq_enable(adapter);
		
#endif /* HAVE_VLAN_RX_REGISTER */
		/* remove vlan from VLVF table array */
		err = igb_vlvf_set(adapter, vid, FALSE, pf_id);
		
		/* if vid was not present in VLVF just remove it from table */
		if (err)
			igb_vfta_set(adapter, vid, FALSE);
#ifndef HAVE_VLAN_RX_REGISTER
		
		clear_bit(vid, adapter->active_vlans);
#endif
#ifdef HAVE_INT_NDO_VLAN_RX_ADD_VID
		return 0;
#endif
	}
#endif

static void igb_restore_vlan(struct igb_adapter *adapter)
{
#ifdef	__APPLE__
	igb_vlan_mode(adapter->netdev, adapter->vlgrp);
#else /* __APPLE__ */
#ifdef HAVE_VLAN_RX_REGISTER
	igb_vlan_mode(adapter->netdev, adapter->vlgrp);
	
	if (adapter->vlgrp) {
		u16 vid;
		for (vid = 0; vid < VLAN_N_VID; vid++) {
			if (!vlan_group_get_device(adapter->vlgrp, vid))
				continue;
#ifdef NETIF_F_HW_VLAN_CTAG_RX
			igb_vlan_rx_add_vid(adapter->netdev,
								htons(ETH_P_8021Q), vid);
#else
			igb_vlan_rx_add_vid(adapter->netdev, vid);
#endif
		}
	}
#else
	u16 vid;
	
	igb_vlan_mode(adapter->netdev, adapter->netdev->features);
	
	for_each_set_bit(vid, adapter->active_vlans, VLAN_N_VID)
#ifdef NETIF_F_HW_VLAN_CTAG_RX
	igb_vlan_rx_add_vid(adapter->netdev,
						htons(ETH_P_8021Q), vid);
#else
	igb_vlan_rx_add_vid(adapter->netdev, vid);
#endif
#endif
#endif  // __APPLE__
}

#ifndef	__APPLE__
/* This function should only be called if RTNL lock is held */
int igb_setup_queues(struct igb_adapter *adapter)
{
	struct net_device *dev = adapter->netdev;
	int err;
	
	if (adapter->rss_queues == adapter->num_rx_queues) {
		if (adapter->tss_queues) {
			if (adapter->tss_queues == adapter->num_tx_queues)
				return 0;
		} else if (adapter->vfs_allocated_count ||
				   adapter->rss_queues == adapter->num_tx_queues) {
			return 0;
		}
	}
	
	/*
	 * Hardware has to reinitialize queues and interrupts to
	 * match the new configuration. Unfortunately, the hardware
	 * is not flexible enough to do this dynamically.
	 */
	if (netif_running(dev))
		igb_close(dev);
	
	igb_clear_interrupt_scheme(adapter);
	
	err = igb_init_interrupt_scheme(adapter, true);
	if (err) {
		dev_close(dev);
		return err;
	}
	
	if (netif_running(dev))
		err = igb_open(dev);
	
	return err;
}

static int __igb_shutdown(struct pci_dev *pdev, bool *enable_wake,
							bool runtime)
{
	IOEthernetController *netdev = pci_get_drvdata(pdev);
	struct igb_adapter *adapter = netdev_priv(netdev);
	struct e1000_hw *hw = &adapter->hw;
	u32 ctrl, rctl, status;
	u32 wufc = runtime ? E1000_WUFC_LNKC : adapter->wol;
#ifdef CONFIG_PM
	int retval = 0;
#endif

	netif_device_detach(netdev);

	status = E1000_READ_REG(hw, E1000_STATUS);
	if (status & E1000_STATUS_LU)
		wufc &= ~E1000_WUFC_LNKC;

	if (netif_running(netdev))
		__igb_close(netdev, true);
	
	igb_clear_interrupt_scheme(adapter);
	
#ifdef CONFIG_PM
	retval = pci_save_state(pdev);
	if (retval)
		return retval;
#endif

	if (wufc) {
		igb_setup_rctl(adapter);
		igb_set_rx_mode(netdev);

		/* turn on all-multi mode if wake on multicast is enabled */
		if (wufc & E1000_WUFC_MC) {
			rctl = E1000_READ_REG(hw, E1000_RCTL);
			rctl |= E1000_RCTL_MPE;
			E1000_WRITE_REG(hw, E1000_RCTL, rctl);
		}

		ctrl = E1000_READ_REG(hw, E1000_CTRL);
		/* phy power management enable */
		#define E1000_CTRL_EN_PHY_PWR_MGMT 0x00200000
		ctrl |= E1000_CTRL_ADVD3WUC;
		E1000_WRITE_REG(hw, E1000_CTRL, ctrl);

		/* Allow time for pending master requests to run */
		e1000_disable_pcie_master(hw);

		E1000_WRITE_REG(hw, E1000_WUC, E1000_WUC_PME_EN);
		E1000_WRITE_REG(hw, E1000_WUFC, wufc);
	} else {
		E1000_WRITE_REG(hw, E1000_WUC, 0);
		E1000_WRITE_REG(hw, E1000_WUFC, 0);
	}

	*enable_wake = wufc || adapter->en_mng_pt;
	if (!*enable_wake)
		igb_power_down_link(adapter);
	else
		igb_power_up_link(adapter);

	/* Release control of h/w to f/w.  If f/w is AMT enabled, this
	 * would have already happened in close and is redundant.
	 */
	igb_release_hw_control(adapter);

	pci_disable_device(pdev);

	return 0;
}
#endif

#ifdef CONFIG_PM
#ifdef HAVE_SYSTEM_SLEEP_PM_OPS
static int igb_suspend(IOPCIDevice *pdev, pm_message_t state)
#else
static int igb_suspend(struct pci_dev *pdev, pm_message_t state)
#endif /* HAVE_SYSTEM_SLEEP_PM_OPS */
{
#ifdef HAVE_SYSTEM_SLEEP_PM_OPS
	struct pci_dev *pdev = to_pci_dev(dev);
#endif /* HAVE_SYSTEM_SLEEP_PM_OPS */
	int retval;
	bool wake;

	retval = __igb_shutdown(pdev, &wake, 0);
	if (retval)
		return retval;

	if (wake) {
		pci_prepare_to_sleep(pdev);
	} else {
		pci_wake_from_d3(pdev, false);
		pci_set_power_state(pdev, PCI_D3hot);
	}

	return 0;
}

#ifdef HAVE_SYSTEM_SLEEP_PM_OPS
static int igb_resume(struct device *dev)
#else
static int igb_resume(struct pci_dev *pdev)
#endif /* HAVE_SYSTEM_SLEEP_PM_OPS */
{
#ifdef HAVE_SYSTEM_SLEEP_PM_OPS
	struct pci_dev *pdev = to_pci_dev(dev);
#endif /* HAVE_SYSTEM_SLEEP_PM_OPS */
	IOEthernetController *netdev = pci_get_drvdata(pdev);
	struct igb_adapter *adapter = netdev_priv(netdev);
	struct e1000_hw *hw = &adapter->hw;
	u32 err;

	pci_set_power_state(pdev, PCI_D0);
	pci_restore_state(pdev);
	pci_save_state(pdev);

	err = pci_enable_device_mem(pdev);
	if (err) {
		pr_err("igb: Cannot enable PCI device from suspend\n");
		return err;
	}
	pci_set_master(pdev);

	pci_enable_wake(pdev, PCI_D3hot, 0);
	pci_enable_wake(pdev, PCI_D3cold, 0);

	if (igb_init_interrupt_scheme(adapter, true)) {
			pr_err("Unable to allocate memory for queues\n");
		return -ENOMEM;
	}

	igb_reset(adapter);

	/* let the f/w know that the h/w is now under the control of the
	 * driver.
	 */
	igb_get_hw_control(adapter);

	E1000_WRITE_REG(hw, E1000_WUS, ~0);

	if (netdev->flags & IFF_UP) {
		rtnl_lock();
		err = __igb_open(netdev, true);
		rtnl_unlock();
		if (err)
			return err;
	}

	netif_device_attach(netdev);

	return 0;
}
#ifdef CONFIG_PM_RUNTIME
#ifdef HAVE_SYSTEM_SLEEP_PM_OPS
static int igb_runtime_idle(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct net_device *netdev = pci_get_drvdata(pdev);
	struct igb_adapter *adapter = netdev_priv(netdev);
	
	if (!igb_has_link(adapter))
		pm_schedule_suspend(dev, MSEC_PER_SEC * 5);
	
	return -EBUSY;
}

static int igb_runtime_suspend(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	int retval;
	bool wake;
	
	retval = __igb_shutdown(pdev, &wake, 1);
	if (retval)
		return retval;
	
	if (wake) {
		pci_prepare_to_sleep(pdev);
	} else {
		pci_wake_from_d3(pdev, false);
		pci_set_power_state(pdev, PCI_D3hot);
	}
	
	return 0;
}
	
static int igb_runtime_resume(struct device *dev)
{
	return igb_resume(dev);
}
#endif /* HAVE_SYSTEM_SLEEP_PM_OPS */
#endif /* CONFIG_PM_RUNTIME */
#endif	/* CONFIG_PM */

#ifndef	__APPLE__
#ifdef USE_REBOOT_NOTIFIER
/* only want to do this for 2.4 kernels? */
static int igb_notify_reboot(struct notifier_block *nb, unsigned long event,
                             void *p)
{
	IOPCIDevice *pdev = NULL;
	bool wake;

	switch (event) {
	case SYS_DOWN:
	case SYS_HALT:
	case SYS_POWER_OFF:
		while ((pdev = pci_find_device(PCI_ANY_ID, PCI_ANY_ID, pdev))) {
			if (pci_dev_driver(pdev) == &igb_driver) {
				__igb_shutdown(pdev, &wake, 0);
				if (event == SYS_POWER_OFF) {
					pci_wake_from_d3(pdev, wake);
					pci_set_power_state(pdev, PCI_D3hot);
				}
			}
		}
	}
	return NOTIFY_DONE;
}
#else
static void igb_shutdown(IOPCIDevice *pdev)
{
	bool wake;

	__igb_shutdown(pdev, &wake, 0);

	if (system_state == SYSTEM_POWER_OFF) {
		pci_wake_from_d3(pdev, wake);
		pci_set_power_state(pdev, PCI_D3hot);
	}
}
#endif /* USE_REBOOT_NOTIFIER */
#endif

int igb_add_mac_filter(struct igb_adapter *adapter, u8 *addr, u16 queue)
{
	struct e1000_hw *hw = &adapter->hw;
	int i;

	if (is_zero_ether_addr(addr))
		return 0;

	for (i = 0; i < hw->mac.rar_entry_count; i++) {
		if (adapter->mac_table[i].state & IGB_MAC_STATE_IN_USE)
			continue;
		adapter->mac_table[i].state = (IGB_MAC_STATE_MODIFIED |
						   IGB_MAC_STATE_IN_USE);
		memcpy(adapter->mac_table[i].addr, addr, ETH_ALEN);
		adapter->mac_table[i].queue = queue;
		igb_sync_mac_table(adapter);
		return 0;
	}
	return -ENOMEM;
}
int igb_del_mac_filter(struct igb_adapter *adapter, u8* addr, u16 queue)
{
	/* search table for addr, if found, set to 0 and sync */
	int i;
	struct e1000_hw *hw = &adapter->hw;

	if (is_zero_ether_addr(addr))
		return 0;
	for (i = 0; i < hw->mac.rar_entry_count; i++) {
		if (!ether_addr_equal(addr, adapter->mac_table[i].addr) &&
		    adapter->mac_table[i].queue == queue) {
			adapter->mac_table[i].state = IGB_MAC_STATE_MODIFIED;
			memset(adapter->mac_table[i].addr, 0, ETH_ALEN);
			adapter->mac_table[i].queue = 0;
			igb_sync_mac_table(adapter);
			return 0;
		}
	}
	return -ENOMEM;
}
static int igb_set_vf_mac(struct igb_adapter *adapter,
                          int vf, unsigned char *mac_addr)
{
	struct e1000_hw *hw = &adapter->hw;

	/* VF MAC addresses start at end of receive addresses and moves
	 * towards the first, as a result a collision should not be possible
	 */
	int rar_entry = hw->mac.rar_entry_count - (vf + 1);
	
	memcpy(adapter->vf_data[vf].vf_mac_addresses, mac_addr, ETH_ALEN);
	
	igb_rar_set_qsel(adapter, mac_addr, rar_entry, vf);

	return 0;
}

#ifdef IFLA_VF_MAX
static int igb_ndo_set_vf_mac(IOEthernetController *netdev, int vf, u8 *mac)
{
	struct igb_adapter *adapter = netdev_priv(netdev);
	if (!is_valid_ether_addr(mac) || (vf >= adapter->vfs_allocated_count))
		return -EINVAL;
	adapter->vf_data[vf].flags |= IGB_VF_FLAG_PF_SET_MAC;
	dev_info(&adapter->pdev->dev, "setting MAC %pM on VF %d\n", mac, vf);
	dev_info(&adapter->pdev->dev, "Reload the VF driver to make this"
				      " change effective.\n");
	if (test_bit(__IGB_DOWN, &adapter->state)) {
		dev_warn(&adapter->pdev->dev, "The VF MAC address has been set,"
			 " but the PF device is not up.\n");
		dev_warn(&adapter->pdev->dev, "Bring the PF device up before"
			 " attempting to use the VF device.\n");
	}
	return igb_set_vf_mac(adapter, vf, mac);
}

static int igb_link_mbps(int internal_link_speed)
{
	switch (internal_link_speed) {
	case SPEED_100:
		return 100;
	case SPEED_1000:
		return 1000;
	case SPEED_2500:
		return 2500;
	default:
		return 0;
	}
}

static void igb_set_vf_rate_limit(struct e1000_hw *hw, int vf, int tx_rate,
			int link_speed)
{
	int rf_dec, rf_int;
	u32 bcnrc_val;

	if (tx_rate != 0) {
		/* Calculate the rate factor values to set */
		rf_int = link_speed / tx_rate;
		rf_dec = (link_speed - (rf_int * tx_rate));
		rf_dec = (rf_dec * (1<<E1000_RTTBCNRC_RF_INT_SHIFT)) / tx_rate;

		bcnrc_val = E1000_RTTBCNRC_RS_ENA;
		bcnrc_val |= ((rf_int<<E1000_RTTBCNRC_RF_INT_SHIFT) &
				E1000_RTTBCNRC_RF_INT_MASK);
		bcnrc_val |= (rf_dec & E1000_RTTBCNRC_RF_DEC_MASK);
	} else {
		bcnrc_val = 0;
	}

	E1000_WRITE_REG(hw, E1000_RTTDQSEL, vf); /* vf X uses queue X */
	/*
	 * Set global transmit compensation time to the MMW_SIZE in RTTBCNRM
	 * register. MMW_SIZE=0x014 if 9728-byte jumbo is supported.
	 */
	E1000_WRITE_REG(hw, E1000_RTTBCNRM(0), 0x14);
	E1000_WRITE_REG(hw, E1000_RTTBCNRC, bcnrc_val);
}

static void igb_check_vf_rate_limit(struct igb_adapter *adapter)
{
	int actual_link_speed, i;
	bool reset_rate = false;

	/* VF TX rate limit was not set */
	if ((adapter->vf_rate_link_speed == 0) ||
		(adapter->hw.mac.type != e1000_82576))
		return;

	actual_link_speed = igb_link_mbps(adapter->link_speed);
	if (actual_link_speed != adapter->vf_rate_link_speed) {
		reset_rate = true;
		adapter->vf_rate_link_speed = 0;
		dev_info(&adapter->pdev->dev,
		"Link speed has been changed. VF Transmit rate is disabled\n");
	}

	for (i = 0; i < adapter->vfs_allocated_count; i++) {
		if (reset_rate)
			adapter->vf_data[i].tx_rate = 0;

		igb_set_vf_rate_limit(&adapter->hw, i,
			adapter->vf_data[i].tx_rate, actual_link_speed);
	}
}

#ifdef HAVE_NDO_SET_VF_MIN_MAX_TX_RATE
static int igb_ndo_set_vf_bw(IOEthernetController *netdev, int vf, int min_tx_rate,
							 int max_tx_rate)
#else
static int igb_ndo_set_vf_bw(IOEthernetController *netdev, int vf, int tx_rate)
#endif /* HAVE_NDO_SET_VF_MIN_MAX_TX_RATE */
{
	struct igb_adapter *adapter = netdev_priv(netdev);
	struct e1000_hw *hw = &adapter->hw;
	int actual_link_speed;

	if (hw->mac.type != e1000_82576)
		return -EOPNOTSUPP;

	actual_link_speed = igb_link_mbps(adapter->link_speed);
	if ((vf >= adapter->vfs_allocated_count) ||
		(!(E1000_READ_REG(hw, E1000_STATUS) & E1000_STATUS_LU)) ||
#ifdef HAVE_NDO_SET_VF_MIN_MAX_TX_RATE
		(max_tx_rate < 0) || (max_tx_rate > actual_link_speed))
#else
		(tx_rate < 0) || (tx_rate > actual_link_speed))
#endif /* HAVE_NDO_SET_VF_MIN_MAX_TX_RATE */
		return -EINVAL;

	adapter->vf_rate_link_speed = actual_link_speed;
#ifdef HAVE_NDO_SET_VF_MIN_MAX_TX_RATE
	adapter->vf_data[vf].tx_rate = (u16)max_tx_rate;
	igb_set_vf_rate_limit(hw, vf, max_tx_rate, actual_link_speed);
#else
	adapter->vf_data[vf].tx_rate = (u16)tx_rate;
	igb_set_vf_rate_limit(hw, vf, tx_rate, actual_link_speed);
#endif /* HAVE_NDO_SET_VF_MIN_MAX_TX_RATE */
	
	return 0;
}

static int igb_ndo_get_vf_config(IOEthernetController *netdev,
				 int vf, struct ifla_vf_info *ivi)
{
	struct igb_adapter *adapter = netdev_priv(netdev);
	if (vf >= adapter->vfs_allocated_count)
		return -EINVAL;
	ivi->vf = vf;
	memcpy(&ivi->mac, adapter->vf_data[vf].vf_mac_addresses, ETH_ALEN);
#ifdef HAVE_NDO_SET_VF_MIN_MAX_TX_RATE
	ivi->max_tx_rate = adapter->vf_data[vf].tx_rate;
	ivi->min_tx_rate = 0;
#else
	ivi->tx_rate = adapter->vf_data[vf].tx_rate;
#endif /* HAVE_NDO_SET_VF_MIN_MAX_TX_RATE */
	ivi->vlan = adapter->vf_data[vf].pf_vlan;
	ivi->qos = adapter->vf_data[vf].pf_qos;
#ifdef HAVE_VF_SPOOFCHK_CONFIGURE
	ivi->spoofchk = adapter->vf_data[vf].spoofchk_enabled;
#endif
	return 0;
}
#endif
static void igb_vmm_control(struct igb_adapter *adapter)
{
	struct e1000_hw *hw = &adapter->hw;
	int count;
	u32 reg;

	switch (hw->mac.type) {
	case e1000_82575:
	default:
		/* replication is not supported for 82575 */
		return;
	case e1000_82576:
		/* notify HW that the MAC is adding vlan tags */
		reg = E1000_READ_REG(hw, E1000_DTXCTL);
		reg |= (E1000_DTXCTL_VLAN_ADDED |
			E1000_DTXCTL_SPOOF_INT);
		E1000_WRITE_REG(hw, E1000_DTXCTL, reg);
		/* Fall through */
	case e1000_82580:
		/* enable replication vlan tag stripping */
		reg = E1000_READ_REG(hw, E1000_RPLOLR);
		reg |= E1000_RPLOLR_STRVLAN;
		E1000_WRITE_REG(hw, E1000_RPLOLR, reg);
		/* Fall through */
	case e1000_i350:
	case e1000_i354:
		/* none of the above registers are supported by i350 */
		break;
	}

	/* Enable Malicious Driver Detection */
	if ((adapter->vfs_allocated_count) &&
	    (adapter->mdd)) {
		if (hw->mac.type == e1000_i350)
			igb_enable_mdd(adapter);
	}

	/* enable replication and loopback support */
	count = adapter->vfs_allocated_count || adapter->vmdq_pools;
	if (adapter->flags & IGB_FLAG_LOOPBACK_ENABLE && count)
		e1000_vmdq_set_loopback_pf(hw, 1);

	e1000_vmdq_set_anti_spoofing_pf(hw, adapter->vfs_allocated_count ||
					adapter->vmdq_pools,
					adapter->vfs_allocated_count);
	e1000_vmdq_set_replication_pf(hw, adapter->vfs_allocated_count ||
				      adapter->vmdq_pools);
}

static void igb_init_fw(struct igb_adapter *adapter)
{
	struct e1000_fw_drv_info fw_cmd;
	struct e1000_hw *hw = &adapter->hw;
	int i;
	u16 mask;
	
	if (hw->mac.type == e1000_i210)
		mask = E1000_SWFW_EEP_SM;
	else
		mask = E1000_SWFW_PHY0_SM;
	/* i211 parts do not support this feature */
	if (hw->mac.type == e1000_i211)
		hw->mac.arc_subsystem_valid = false;

	if (!hw->mac.ops.acquire_swfw_sync(hw, mask)) {
		for (i = 0; i <= FW_MAX_RETRIES; i++) {
			E1000_WRITE_REG(hw, E1000_FWSTS, E1000_FWSTS_FWRI);
			fw_cmd.hdr.cmd = FW_CMD_DRV_INFO;
			fw_cmd.hdr.buf_len = FW_CMD_DRV_INFO_LEN;
			fw_cmd.hdr.cmd_or_resp.cmd_resv = FW_CMD_RESERVED;
			fw_cmd.port_num = hw->bus.func;
			fw_cmd.drv_version = FW_FAMILY_DRV_VER;
			fw_cmd.hdr.checksum = 0;
			fw_cmd.hdr.checksum = e1000_calculate_checksum((u8 *)&fw_cmd,
			                                           (FW_HDR_LEN +
			                                            fw_cmd.hdr.buf_len));
			 e1000_host_interface_command(hw, (u8*)&fw_cmd,
			                             sizeof(fw_cmd));
			if (fw_cmd.hdr.cmd_or_resp.ret_status == FW_STATUS_SUCCESS)
				break;
		}
	} else
		pr_err( "Unable to get semaphore, firmware init failed.\n");
	hw->mac.ops.release_swfw_sync(hw, mask);
}

static void igb_init_dmac(struct igb_adapter *adapter, u32 pba)
{
	struct e1000_hw *hw = &adapter->hw;
	u32 dmac_thr;
	u16 hwm;
	u32 status;

	if (hw->mac.type == e1000_i211)
		return;
	
	if (hw->mac.type > e1000_82580) {
		if (adapter->dmac != IGB_DMAC_DISABLE) {
			u32 reg;

			/* force threshold to 0.  */
			E1000_WRITE_REG(hw, E1000_DMCTXTH, 0);

			/*
			 * DMA Coalescing high water mark needs to be greater
			 * than the Rx threshold. Set hwm to PBA - max frame
			 * size in 16B units, capping it at PBA - 6KB.
			 */
			hwm = 64 * pba - adapter->max_frame_size / 16;
			if (hwm < 64 * (pba - 6))
				hwm = 64 * (pba - 6);
			reg = E1000_READ_REG(hw, E1000_FCRTC);
			reg &= ~E1000_FCRTC_RTH_COAL_MASK;
			reg |= ((hwm << E1000_FCRTC_RTH_COAL_SHIFT)
					& E1000_FCRTC_RTH_COAL_MASK);
			E1000_WRITE_REG(hw, E1000_FCRTC, reg);
			
			/*
			 * Set the DMA Coalescing Rx threshold to PBA - 2 * max
			 * frame size, capping it at PBA - 10KB.
			 */
			dmac_thr = pba - adapter->max_frame_size / 512;
			if (dmac_thr < pba - 10)
				dmac_thr = pba - 10;
			reg = E1000_READ_REG(hw, E1000_DMACR);
			reg &= ~E1000_DMACR_DMACTHR_MASK;
			reg |= ((dmac_thr << E1000_DMACR_DMACTHR_SHIFT)
				& E1000_DMACR_DMACTHR_MASK);

			/* transition to L0x or L1 if available..*/
			reg |= (E1000_DMACR_DMAC_EN | E1000_DMACR_DMAC_LX_MASK);

			/* Check if status is 2.5Gb backplane connection
			 * before configuration of watchdog timer, which is
			 * in msec values in 12.8usec intervals
			 * watchdog timer= msec values in 32usec intervals
			 * for non 2.5Gb connection
			 */
			if (hw->mac.type == e1000_i354) {
				status = E1000_READ_REG(hw, E1000_STATUS);
				if ((status & E1000_STATUS_2P5_SKU) &&
				    (!(status & E1000_STATUS_2P5_SKU_OVER)))
					reg |= ((adapter->dmac * 5) >> 6);
				else
					reg |= ((adapter->dmac) >> 5);
			} else {
				reg |= ((adapter->dmac) >> 5);
			}
			
			/*
			 * Disable BMC-to-OS Watchdog enable
			 * on devices that support OS-to-BMC
			 */
			if (hw->mac.type != e1000_i354)
				reg &= ~E1000_DMACR_DC_BMC2OSW_EN;
			E1000_WRITE_REG(hw, E1000_DMACR, reg);

			/* no lower threshold to disable coalescing
			 * (smart fifb)-UTRESH=0
			 */
			E1000_WRITE_REG(hw, E1000_DMCRTRH, 0);

			/* This sets the time to wait before requesting
			 * transition to low power state to number of usecs
			 * needed to receive 1 512 byte frame at gigabit
			 * line rate. On i350 device, time to make transition
			 * to Lx state is delayed by 4 usec with flush disable
			 * bit set to avoid losing mailbox interrupts
			 */
			reg = E1000_READ_REG(hw, E1000_DMCTLX);
			if (hw->mac.type == e1000_i350)
				reg |= IGB_DMCTLX_DCFLUSH_DIS;
			
			/* in 2.5Gb connection, TTLX unit is 0.4 usec
			 * which is 0x4*2 = 0xA. But delay is still 4 usec
			 */
			if (hw->mac.type == e1000_i354) {
				status = E1000_READ_REG(hw, E1000_STATUS);
				if ((status & E1000_STATUS_2P5_SKU) &&
				    (!(status & E1000_STATUS_2P5_SKU_OVER)))
					reg |= 0xA;
				else
					reg |= 0x4;
			} else {
				reg |= 0x4;
			}
			E1000_WRITE_REG(hw, E1000_DMCTLX, reg);

			/* free space in tx pkt buffer to wake from DMA coal */
			E1000_WRITE_REG(hw, E1000_DMCTXTH, (IGB_MIN_TXPBSIZE -
				(IGB_TX_BUF_4096 + adapter->max_frame_size)) >> 6);

			/* make low power state decision controlled by DMA coal */
			reg = E1000_READ_REG(hw, E1000_PCIEMISC);
			reg &= ~E1000_PCIEMISC_LX_DECISION;
			E1000_WRITE_REG(hw, E1000_PCIEMISC, reg);
		} /* endif adapter->dmac is not disabled */
	} else if (hw->mac.type == e1000_82580) {
		u32 reg = E1000_READ_REG(hw, E1000_PCIEMISC);
		E1000_WRITE_REG(hw, E1000_PCIEMISC,
		                reg & ~E1000_PCIEMISC_LX_DECISION);
		E1000_WRITE_REG(hw, E1000_DMACR, 0);
	}
}
	
#ifdef HAVE_I2C_SUPPORT
/*  igb_read_i2c_byte - Reads 8 bit word over I2C
 *  @hw: pointer to hardware structure
 *  @byte_offset: byte offset to read
 *  @dev_addr: device address
 *  @data: value read
 *
 *  Performs byte read operation over I2C interface at
 *  a specified device address.
 */
s32 igb_read_i2c_byte(struct e1000_hw *hw, u8 byte_offset,
					  u8 dev_addr, u8 *data)
{
	struct igb_adapter *adapter = container_of(hw, struct igb_adapter, hw);
	struct i2c_client *this_client = adapter->i2c_client;
	s32 status;
	u16 swfw_mask = 0;
	
	if (!this_client)
		return E1000_ERR_I2C;
	
	swfw_mask = E1000_SWFW_PHY0_SM;
	
	if (hw->mac.ops.acquire_swfw_sync(hw, swfw_mask)
		!= E1000_SUCCESS)
		return E1000_ERR_SWFW_SYNC;
	
	status = i2c_smbus_read_byte_data(this_client, byte_offset);
	hw->mac.ops.release_swfw_sync(hw, swfw_mask);
	
	if (status < 0)
		return E1000_ERR_I2C;
	else {
		*data = status;
		return E1000_SUCCESS;
	}
}

/*  igb_write_i2c_byte - Writes 8 bit word over I2C
 *  @hw: pointer to hardware structure
 *  @byte_offset: byte offset to write
 *  @dev_addr: device address
 *  @data: value to write
 *
 *  Performs byte write operation over I2C interface at
 *  a specified device address.
 */
s32 igb_write_i2c_byte(struct e1000_hw *hw, u8 byte_offset,
					   u8 dev_addr, u8 data)
{
	struct igb_adapter *adapter = container_of(hw, struct igb_adapter, hw);
	struct i2c_client *this_client = adapter->i2c_client;
	s32 status;
	u16 swfw_mask = E1000_SWFW_PHY0_SM;
	
	if (!this_client)
		return E1000_ERR_I2C;
	
	if (hw->mac.ops.acquire_swfw_sync(hw, swfw_mask) != E1000_SUCCESS)
		return E1000_ERR_SWFW_SYNC;
	status = i2c_smbus_write_byte_data(this_client, byte_offset, data);
	hw->mac.ops.release_swfw_sync(hw, swfw_mask);
	
	if (status)
		return E1000_ERR_I2C;
	else
		return E1000_SUCCESS;
}
#endif /*  HAVE_I2C_SUPPORT */

		
int igb_reinit_queues(struct igb_adapter *adapter)
{
	struct IOEthernetController *netdev = adapter->netdev;
	int err = 0;
	
	if (netif_running(netdev))
		igb_close(netdev);
	
	igb_reset_interrupt_capability(adapter);
	
	if (igb_init_interrupt_scheme(adapter, true)) {
		pr_err("Unable to allocate memory for queues\n");
		return -ENOMEM;
	}
	
	if (netif_running(netdev))
		err = igb_open(netdev);
	
	return err;
}
		
		
/* igb_main.c */
#ifdef APPLE_OS_LOG
os_log_t igb_logger = OS_LOG_DEFAULT;
#endif

static IOMediumType mediumTypeArray[MEDIUM_INDEX_COUNT] = {
        kIOMediumEthernetAuto,
        (kIOMediumEthernet10BaseT | kIOMediumOptionHalfDuplex),
        (kIOMediumEthernet10BaseT | kIOMediumOptionFullDuplex),
        (kIOMediumEthernet100BaseTX | kIOMediumOptionHalfDuplex),
        (kIOMediumEthernet100BaseTX | kIOMediumOptionFullDuplex),
        (kIOMediumEthernet100BaseTX | kIOMediumOptionFullDuplex | kIOMediumOptionFlowControl),
        (kIOMediumEthernet1000BaseT | kIOMediumOptionFullDuplex),
        (kIOMediumEthernet1000BaseT | kIOMediumOptionFullDuplex | kIOMediumOptionFlowControl),
        (kIOMediumEthernet1000BaseT | kIOMediumOptionFullDuplex | kIOMediumOptionEEE),
        (kIOMediumEthernet1000BaseT | kIOMediumOptionFullDuplex | kIOMediumOptionFlowControl | kIOMediumOptionEEE),
        (kIOMediumEthernet100BaseTX | kIOMediumOptionFullDuplex | kIOMediumOptionEEE),
        (kIOMediumEthernet100BaseTX | kIOMediumOptionFullDuplex | kIOMediumOptionFlowControl | kIOMediumOptionEEE)
};

static UInt32 mediumSpeedArray[MEDIUM_INDEX_COUNT] = {
        0,
        10 * MBit,
        10 * MBit,
        100 * MBit,
        100 * MBit,
        100 * MBit,
        1000 * MBit,
        1000 * MBit,
        1000 * MBit,
        1000 * MBit,
        100 * MBit,
        100 * MBit
};

static const struct  {
        UInt16 id;
        const char* name;
} deviceModelNames[] =
{
    { E1000_DEV_ID_I354_BACKPLANE_1GBPS, "i354" },
    { E1000_DEV_ID_I354_SGMII, "i354 SGMII" },
    { E1000_DEV_ID_I354_BACKPLANE_2_5GBPS, "i354 2.5G" },
    { E1000_DEV_ID_I210_COPPER, "i210 Copper" },
    { E1000_DEV_ID_I210_FIBER, "i210 Fiber" },
    { E1000_DEV_ID_I210_SERDES, "i210 SerDes" },
    { E1000_DEV_ID_I210_SGMII, "i210 SGMII" },
    { E1000_DEV_ID_I210_COPPER_FLASHLESS, "i210 Copper" },
    { E1000_DEV_ID_I210_SERDES_FLASHLESS, "i210 SerDes" },
    { E1000_DEV_ID_I211_COPPER, "i211 Copper" },
    { E1000_DEV_ID_I350_COPPER, "i350 Copper"},
    { E1000_DEV_ID_I350_FIBER, "i350 Fiber"},
    { E1000_DEV_ID_I350_SERDES, "i350 SerDes"},
    { E1000_DEV_ID_I350_SGMII, "i350 SGMII"},
    { E1000_DEV_ID_82580_COPPER, "82580 Copper"},
    { E1000_DEV_ID_82580_FIBER, "82580 Fiber"},
    { E1000_DEV_ID_82580_QUAD_FIBER, "82580 Quad Fiber"},
    { E1000_DEV_ID_82580_SERDES, "82580 SerDes"},
    { E1000_DEV_ID_82580_SGMII, "82580 SGMII"},
    { E1000_DEV_ID_82580_COPPER_DUAL, "82580 Dual Copper"},
    { E1000_DEV_ID_DH89XXCC_SGMII, "DH89XXCC SGMII"},
    { E1000_DEV_ID_DH89XXCC_SERDES, "DH89XXCC SerDes"},
    { E1000_DEV_ID_DH89XXCC_BACKPLANE, "DH89XXCC Backplane"},
    { E1000_DEV_ID_DH89XXCC_SFP, "DH89XXCC SFP"},
    { E1000_DEV_ID_82576, "82576"},
    { E1000_DEV_ID_82576_NS, "82576 NS"},
    { E1000_DEV_ID_82576_NS_SERDES, "82576 NS SerDes"},
    { E1000_DEV_ID_82576_FIBER, "82576 Fiber"},
    { E1000_DEV_ID_82576_SERDES, "82576 SerDes"},
    { E1000_DEV_ID_82576_SERDES_QUAD, "82576 Quad SerDes"},
    { E1000_DEV_ID_82576_QUAD_COPPER_ET2, "82576 Quad Copper ET2"},
    { E1000_DEV_ID_82576_QUAD_COPPER, "82576 Quad Copper"},
    { E1000_DEV_ID_82575EB_COPPER, "82575EB Copper"},
    { E1000_DEV_ID_82575EB_FIBER_SERDES, "82575EB Fiber SerDes"},
    { E1000_DEV_ID_82575GB_QUAD_COPPER, "82575 Quad Copper"},
};

OSDefineMetaClassAndStructors(AppleIGB, super);


void AppleIGB::free()
{
	RELEASE(mediumDict);
	
	super::free();
}

bool AppleIGB::setupMediumDict()
{
        IONetworkMedium *medium;
        UInt32 count;
        UInt32 i;
        bool result = false;

        pr_debug("setupMediumDict() ===>\n");

        if (priv_adapter.hw.phy.media_type == e1000_media_type_fiber) {
            count = 1;
        } else if (intelSupportsEEE(&priv_adapter)) {
            count = MEDIUM_INDEX_COUNT;
        } else {
            count = MEDIUM_INDEX_COUNT - 4;
        }

        mediumDict = OSDictionary::withCapacity(count + 1);

        if (mediumDict) {
            for (i = MEDIUM_INDEX_AUTO; i < count; i++) {
                medium = IONetworkMedium::medium(mediumTypeArray[i], mediumSpeedArray[i], 0, i);

                if (!medium)
                    goto error1;

                result = IONetworkMedium::addMedium(mediumDict, medium);
                medium->release();

                if (!result)
                    goto error1;

                mediumTable[i] = medium;
            }
        }
        result = publishMediumDictionary(mediumDict);

        if (!result)
            goto error1;

    done:
        pr_debug("setupMediumDict() <===\n");
        return result;

    error1:
        pr_err("Error creating medium dictionary.\n");
        mediumDict->release();

        for (i = MEDIUM_INDEX_AUTO; i < MEDIUM_INDEX_COUNT; i++)
            mediumTable[i] = NULL;

        goto done;
}

bool AppleIGB::init(OSDictionary *properties)
{
    #ifdef APPLE_OS_LOG
    igb_logger = os_log_create("com.amdosx.driver.AppleIGB", "Drivers");
    #endif

	if (super::init(properties) == false) 
		return false;
		
	enabledForNetif = false;
    workLoop = NULL;

	pdev = NULL;
	mediumDict = NULL;
	csrPCIAddress = NULL;
	interruptSource = NULL;
	watchdogSource = NULL;
    resetSource = NULL;
    dmaErrSource = NULL;

	netif = NULL;
	
	transmitQueue = NULL;
	preLinkStatus = 0;
	txMbufCursor = NULL;
	bSuspended = FALSE;

    linkUp = FALSE;
    stalled = FALSE;

    eeeMode = 0;

	_mtu = 1500;

	return true;
}

// follows after igb_remove()
void AppleIGB::igb_remove()
{
	struct igb_adapter *adapter = &priv_adapter;
	
#ifdef HAVE_I2C_SUPPORT
	igb_remove_i2c(adapter);
#endif /* HAVE_I2C_SUPPORT */
#ifdef HAVE_PTP_1588_CLOCK
	igb_ptp_stop(adapter);
#endif /* HAVE_PTP_1588_CLOCK */
	
	set_bit(__IGB_DOWN, &adapter->state);
	
#ifdef IGB_DCA
	if (adapter->flags & IGB_FLAG_DCA_ENABLED) {
		pr_err("DCA disabled\n");
		dca_remove_requester(&pdev->dev);
		adapter->flags &= ~IGB_FLAG_DCA_ENABLED;
		E1000_WRITE_REG(hw, E1000_DCA_CTRL, E1000_DCA_CTRL_DCA_DISABLE);
	}
#endif
#ifdef CONFIG_IGB_VMDQ_NETDEV
	igb_remove_vmdq_netdevs(adapter);
#endif
	
	igb_reset_sriov_capability(adapter);

	/* Release control of h/w to f/w.  If f/w is AMT enabled, this
	 * would have already happened in close and is redundant.
	 */
	igb_release_hw_control(adapter);
#if	0
	unregister_netdev(netdev);
#endif
	
	igb_clear_interrupt_scheme(adapter);
	igb_reset_sriov_capability(adapter);
	
#if	1
	RELEASE(csrPCIAddress);
#else
	if (adapter->io_addr)
		iounmap(hw->io_addr);
	if (hw->flash_address)
		iounmap(hw->flash_address);
	pci_release_selected_regions(pdev,
	                             pci_select_bars(pdev, IORESOURCE_MEM));
#endif
	kfree(adapter->mac_table, sizeof(struct igb_mac_addr)* adapter->hw.mac.rar_entry_count);
	kfree(adapter->shadow_vfta, sizeof(u32) * E1000_VFTA_ENTRIES);
#if	1
#else
	free_netdev(netdev);
	
	pci_disable_pcie_error_reporting(pdev);
	
	pci_disable_device(pdev);
#endif
}

void AppleIGB::stop(IOService* provider)
{
	DEBUGOUT("stop()\n");
	detachInterface(netif);
	RELEASE(netif);
	/* flush_scheduled work may reschedule our watchdog task, so
	 * explicitly disable watchdog tasks from being rescheduled
	 */
	if(workLoop){
		if (watchdogSource) {
			workLoop->removeEventSource(watchdogSource);
			RELEASE(watchdogSource);
		}
		if (resetSource) {
			workLoop->removeEventSource(resetSource);
			RELEASE(resetSource);
		}
		if (dmaErrSource) {
			workLoop->removeEventSource(dmaErrSource);
			RELEASE(dmaErrSource);
		}
		
		if (interruptSource) {
			workLoop->removeEventSource(interruptSource);
			RELEASE(interruptSource);
		}
		RELEASE(workLoop);
	}

	igb_remove();

	RELEASE(pdev);

    enabledForNetif = false;

	super::stop(provider);
}

	
// igb_probe
bool AppleIGB::igb_probe()
{
    bool success = false;
	struct igb_adapter *adapter = &priv_adapter;
	struct e1000_hw *hw = &adapter->hw;
	u16 eeprom_data = 0;
	u8 pba_str[E1000_PBANUM_LENGTH];
	s32 ret_val;
	int err;
	static SInt8 global_quad_port_a; /* global quad port a indication */
	static SInt8 cards_found;
	

	csrPCIAddress = pdev->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
	if (csrPCIAddress == NULL) {
		return false;
	}
	
	{
		UInt16	reg16;
		reg16	= pdev->configRead16( kIOPCIConfigCommand );
		reg16  &= ~kIOPCICommandIOSpace;
		reg16	|= ( kIOPCICommandBusMaster
					|    kIOPCICommandMemorySpace
					|	 kIOPCICommandMemWrInvalidate );
		
		pdev->configWrite16( kIOPCIConfigCommand, reg16 );
		
		// pdev->setMemoryEnable(true);
	}
	
	do {
#ifndef HAVE_ASPM_QUIRKS
		/* 82575 requires that the pci-e link partner disable the L0s state */
		switch (pdev->configRead16(kIOPCIConfigDeviceID)) {
			case E1000_DEV_ID_82575EB_COPPER:
			case E1000_DEV_ID_82575EB_FIBER_SERDES:
			case E1000_DEV_ID_82575GB_QUAD_COPPER:
				// I do not know how to implement this.
				// pci_disable_link_state(pdev, PCIE_LINK_STATE_L0S);
			default:
				break;
		}
#endif /* HAVE_ASPM_QUIRKS */
#if	0
		err = pci_request_selected_regions(pdev,
										   pci_select_bars(pdev,
														   IORESOURCE_MEM),
										   igb_driver_name);
#endif
		// pdev->setBusMasterEnable(true);
		
		//SET_MODULE_OWNER(netdev);
		//SET_NETDEV_DEV(netdev, &pdev->dev);
		
		//pci_set_drvdata(pdev, netdev);
		adapter->netdev = this;
		adapter->pdev = pdev;
		hw->back = adapter;
		adapter->port_num = hw->bus.func;
		//adapter->msg_enable = (1 << debug) - 1;
		
		adapter->io_addr = (u8*)(csrPCIAddress->getVirtualAddress());
		/* hw->hw_addr can be zeroed, so use adapter->io_addr for unmap */
		hw->hw_addr = adapter->io_addr;

#ifdef HAVE_NET_DEVICE_OPS
		//netdev->netdev_ops = &igb_netdev_ops;
#ifdef HAVE_RHEL6_NET_DEVICE_OPS_EXT
		//set_netdev_ops_ext(netdev, &igb_netdev_ops_ext);
#endif /* HAVE_RHEL6_NET_DEVICE_OPS_EXT */
#else /* HAVE_NET_DEVICE_OPS */
		//netdev->open = &igb_open;
		//netdev->stop = &igb_close;
		//netdev->get_stats = &igb_get_stats;
#ifdef HAVE_SET_RX_MODE
		//netdev->set_rx_mode = &igb_set_rx_mode;
#endif
		//netdev->set_multicast_list = &igb_set_rx_mode;
		//netdev->set_mac_address = &igb_set_mac;
		//netdev->change_mtu = &igb_change_mtu;
		//netdev->do_ioctl = &igb_ioctl;
#ifdef HAVE_TX_TIMEOUT
		//netdev->tx_timeout = &igb_tx_timeout;
#endif
		//netdev->vlan_rx_register = igb_vlan_mode;
		//netdev->vlan_rx_add_vid = igb_vlan_rx_add_vid;
		//netdev->vlan_rx_kill_vid = igb_vlan_rx_kill_vid;
		//netdev->hard_start_xmit = &igb_xmit_frame;
#endif /* HAVE_NET_DEVICE_OPS */
		//igb_set_ethtool_ops(netdev);
#ifdef HAVE_TX_TIMEOUT
		//netdev->watchdog_timeo = 5 * HZ;
#endif
		
		//strncpy(netdev->name, pci_name(pdev), sizeof(netdev->name) - 1);
		
		adapter->bd_number = OSIncrementAtomic8(&cards_found);
		
		/* setup the private structure */
		err = igb_sw_init(adapter);
		if (err)
			goto err_sw_init;
		
		e1000_get_bus_info(hw);
		
		hw->phy.autoneg_wait_to_complete = FALSE;
		hw->mac.adaptive_ifs = FALSE;
		
		/* Copper options */
		if (hw->phy.media_type == e1000_media_type_copper) {
			hw->phy.mdix = AUTO_ALL_MODES;
			hw->phy.disable_polarity_correction = FALSE;
			hw->phy.ms_type = e1000_ms_hw_default;
		}
		
		if (e1000_check_reset_block(hw))
			pr_err("PHY reset is blocked due to SOL/IDER session.\n");
		
		/*
		 * features is initialized to 0 in allocation, it might have bits
		 * set by igb_sw_init so we should use an or instead of an
		 * assignment.
		 */
		_features |= NETIF_F_SG |
		NETIF_F_IP_CSUM |
#ifdef NETIF_F_IPV6_CSUM
		NETIF_F_IPV6_CSUM |
#endif
#ifdef NETIF_F_RXHASH
		NETIF_F_RXHASH |
#endif
#ifdef HAVE_NDO_SET_FEATURES
		NETIF_F_RXCSUM |
#endif
#ifdef NETIF_F_HW_VLAN_CTAG_RX
        NETIF_F_HW_VLAN_CTAG_RX |
        NETIF_F_HW_VLAN_CTAG_TX;
#else
		NETIF_F_HW_VLAN_RX |
		NETIF_F_HW_VLAN_TX;
#endif
		
        if (hw->mac.type >= e1000_82576)
            _features |= NETIF_F_SCTP_CSUM;
		
#ifdef HAVE_NDO_SET_FEATURES
		/* copy netdev features into list of user selectable features */
		netdev->hw_features |= _features;
#else
#ifdef NETIF_F_GRO
		
		/* this is only needed on kernels prior to 2.6.39 */
		_features |= NETIF_F_GRO;
#endif
#endif
		
		/* set this bit last since it cannot be part of hw_features */
#ifdef NETIF_F_HW_VLAN_CTAG_FILTER
        _features |= NETIF_F_HW_VLAN_CTAG_FILTER;
#else
        _features |= NETIF_F_HW_VLAN_FILTER;
#endif
		
		adapter->en_mng_pt = e1000_enable_mng_pass_thru(hw);
#ifdef DEBUG
		if (adapter->dmac != IGB_DMAC_DISABLE)
			pr_err("DMA Coalescing is enabled..\n");
#endif
		
		/* before reading the NVM, reset the controller to put the device in a
		 * known good starting state */
		e1000_reset_hw(hw);
		
		/* make sure the NVM is good */
		if (e1000_validate_nvm_checksum(hw) < 0) {
			pr_err("The NVM Checksum Is Not Valid\n");
			goto err_eeprom;
		}
		
		/* copy the MAC address out of the NVM */
		if (e1000_read_mac_addr(hw))
            pr_err("NVM Read Error\n");
		
		if (!is_valid_ether_addr(hw->mac.addr)) {
            pr_err("Invalid MAC Address\n");
			goto err_eeprom;
		}
		
		memcpy(&adapter->mac_table[0].addr, hw->mac.addr, ETH_ALEN);
		
		adapter->mac_table[0].queue = adapter->vfs_allocated_count;
		adapter->mac_table[0].state = (IGB_MAC_STATE_DEFAULT
								| IGB_MAC_STATE_IN_USE);
		igb_rar_set(adapter, 0);
		
		/* get firmware version for ethtool -i */
		igb_set_fw_version(adapter);
		
		/* configure RXPBSIZE and TXPBSIZE */
		if (hw->mac.type == e1000_i210) {
			E1000_WRITE_REG(hw, E1000_RXPBS, I210_RXPBSIZE_DEFAULT);
			E1000_WRITE_REG(hw, E1000_TXPBS, I210_TXPBSIZE_DEFAULT);
		}
		
        /* Check if Media Autosense is enabled */
        if (hw->mac.type == e1000_82580)
            igb_init_mas(adapter);
		
		adapter->watchdog_task = watchdogSource;
		if (adapter->flags & IGB_FLAG_DETECT_BAD_DMA)
			adapter->dma_err_task = dmaErrSource;
		adapter->reset_task = resetSource;
		
		/* Initialize link properties that are user-changeable */
		adapter->fc_autoneg = true;
		hw->mac.autoneg = true;
		hw->phy.autoneg_advertised = 0x2f;
		
		hw->fc.requested_mode = e1000_fc_default;
		hw->fc.current_mode = e1000_fc_default;
		
		e1000_validate_mdi_setting(hw);
		
		/* By default, support wake on port A */
		if (hw->bus.func == 0)
			adapter->flags |= IGB_FLAG_WOL_SUPPORTED;
		
		/* Check the NVM for wake support for non-port A ports */
		if (hw->mac.type >= e1000_82580)
			hw->nvm.ops.read(hw, NVM_INIT_CONTROL3_PORT_A +
							 NVM_82580_LAN_FUNC_OFFSET(hw->bus.func), 1,
							 &eeprom_data);
		else if (hw->bus.func == 1)
			e1000_read_nvm(hw, NVM_INIT_CONTROL3_PORT_B, 1, &eeprom_data);
		
		if (eeprom_data & IGB_EEPROM_APME)
			adapter->flags |= IGB_FLAG_WOL_SUPPORTED;
		
		/* now that we have the eeprom settings, apply the special cases where
		 * the eeprom may be wrong or the board simply won't support wake on
		 * lan on a particular port
		 */
		switch (pdev->configRead16(kIOPCIConfigDeviceID)) {
			case E1000_DEV_ID_82575GB_QUAD_COPPER:
				adapter->flags &= ~IGB_FLAG_WOL_SUPPORTED;
				break;
			case E1000_DEV_ID_82575EB_FIBER_SERDES:
			case E1000_DEV_ID_82576_FIBER:
			case E1000_DEV_ID_82576_SERDES:
				/* Wake events only supported on port A for dual fiber
				 * regardless of eeprom setting
				 */
				if (E1000_READ_REG(hw, E1000_STATUS) & E1000_STATUS_FUNC_1)
					adapter->flags &= ~IGB_FLAG_WOL_SUPPORTED;
				break;
			case E1000_DEV_ID_82576_QUAD_COPPER:
			case E1000_DEV_ID_82576_QUAD_COPPER_ET2:
				/* if quad port adapter, disable WoL on all but port A */
				if (global_quad_port_a != 0)
					adapter->flags &= ~IGB_FLAG_WOL_SUPPORTED;
				else
					adapter->flags |= IGB_FLAG_QUAD_PORT_A;
				/* Reset for multiple quad port adapters */
				if (++global_quad_port_a == 4)
					global_quad_port_a = 0;
				break;
			default:
				break;
		}
		
		/* initialize the wol settings based on the eeprom settings */
		if (adapter->flags & IGB_FLAG_WOL_SUPPORTED)
			adapter->wol |= E1000_WUFC_MAG;
/* @todo */
#if	0
		/* Some vendors want the ability to Use the EEPROM setting as
		 * enable/disable only, and not for capability
		 */
		if (((hw->mac.type == e1000_i350) ||
			 (hw->mac.type == e1000_i354)) &&
			(pdev->subsystem_vendor == PCI_VENDOR_ID_DELL)) {
			adapter->flags |= IGB_FLAG_WOL_SUPPORTED;
			adapter->wol = 0;
		}
		if (hw->mac.type == e1000_i350) {
			if (((pdev->subsystem_device == 0x5001) ||
				 (pdev->subsystem_device == 0x5002)) &&
				(hw->bus.func == 0)) {
				adapter->flags |= IGB_FLAG_WOL_SUPPORTED;
				adapter->wol = 0;
			}
			if (pdev->subsystem_device == 0x1F52)
				adapter->flags |= IGB_FLAG_WOL_SUPPORTED;
		}
		device_set_wakeup_enable(pci_dev_to_dev(adapter->pdev),
								 adapter->flags & IGB_FLAG_WOL_SUPPORTED);
#endif
		/* reset the hardware with the new settings */
		igb_reset(adapter);
        adapter->devrc = 0;
		
#ifdef HAVE_I2C_SUPPORT
		/* Init the I2C interface */
		err = igb_init_i2c(adapter);
		if (err) {
			dev_err(&pdev->dev, "failed to init i2c interface\n");
			goto err_eeprom;
		}
#endif /* HAVE_I2C_SUPPORT */
		
		/* let the f/w know that the h/w is now under the control of the
		 * driver.
		 */
		igb_get_hw_control(adapter);
		
		/* carrier off reporting is important to ethtool even BEFORE open */
		netif_carrier_off(this);
		
#ifdef IGB_DCA
		if (dca_add_requester(&pdev->dev) == E1000_SUCCESS) {
			adapter->flags |= IGB_FLAG_DCA_ENABLED;
			dev_info(pci_dev_to_dev(pdev), "DCA enabled\n");
			igb_setup_dca(adapter);
		}
		
#endif
#ifdef HAVE_PTP_1588_CLOCK
		/* do hw tstamp init after resetting */
		igb_ptp_init(adapter);
		
#endif /* HAVE_PTP_1588_CLOCK */
		pr_err("Intel(R) Gigabit Ethernet Network Connection\n");
		/* print bus type/speed/width info */
		pr_err("%s: (PCIe:%s:%s) ",
			  "AppleIGB",
			  ((hw->bus.speed == e1000_bus_speed_2500) ? "2.5GT/s" :
			   (hw->bus.speed == e1000_bus_speed_5000) ? "5.0GT/s" :
			   (hw->mac.type == e1000_i354) ? "integrated" : "unknown"),
			  ((hw->bus.width == e1000_bus_width_pcie_x4) ? "Width x4" :
			   (hw->bus.width == e1000_bus_width_pcie_x2) ? "Width x2" :
			   (hw->bus.width == e1000_bus_width_pcie_x1) ? "Width x1" :
			   (hw->mac.type == e1000_i354) ? "integrated" : "unknown"));
		pr_err("MAC: %2x:%2x:%2x:%2x:%2x:%2x ",
			  hw->mac.addr[0],hw->mac.addr[1],hw->mac.addr[2],
			  hw->mac.addr[3],hw->mac.addr[4],hw->mac.addr[5]);
		ret_val = e1000_read_pba_string(hw, pba_str, E1000_PBANUM_LENGTH);
		pr_err("PBA No: %s\n", ret_val ? "Unknown": (char*)pba_str);
		
		/* Initialize the thermal sensor on i350 devices. */
		if (hw->mac.type == e1000_i350) {
			if (hw->bus.func == 0) {
				u16 ets_word;
				
				/*
				 * Read the NVM to determine if this i350 device
				 * supports an external thermal sensor.
				 */
				e1000_read_nvm(hw, NVM_ETS_CFG, 1, &ets_word);
				if (ets_word != 0x0000 && ets_word != 0xFFFF)
					adapter->ets = true;
				else
					adapter->ets = false;
			}
		} else {
			adapter->ets = false;
		}
		
		if (hw->phy.media_type == e1000_media_type_copper) {
			switch (hw->mac.type) {
				case e1000_i350:
				case e1000_i210:
				case e1000_i211:
					/* Enable EEE for internal copper PHY devices */
					err = e1000_set_eee_i350(hw, true, true);
					if ((!err) &&
						(adapter->flags & IGB_FLAG_EEE))
						adapter->eee_advert =
						MDIO_EEE_100TX | MDIO_EEE_1000T;
					break;
				case e1000_i354:
					if ((E1000_READ_REG(hw, E1000_CTRL_EXT)) &
						(E1000_CTRL_EXT_LINK_MODE_SGMII)) {
						err = e1000_set_eee_i354(hw, true, true);
						if ((!err) &&
							(adapter->flags & IGB_FLAG_EEE))
							adapter->eee_advert =
							MDIO_EEE_100TX | MDIO_EEE_1000T;
					}
					break;
				default:
					break;
			}
		}
		
		/* send driver version info to firmware */
		if ((hw->mac.type >= e1000_i350) &&
			(e1000_get_flash_presence_i210(hw)))
			igb_init_fw(adapter);
		pr_err(
			  "Using %s interrupts. %d rx queue(s), %d tx queue(s)\n",
			  adapter->msix_entries ? "MSI-X" :
			  (adapter->flags & IGB_FLAG_HAS_MSI) ? "MSI" : "legacy",
			  adapter->num_rx_queues, adapter->num_tx_queues);
		
		//cards_found++; -> atomic
		//pm_runtime_put_noidle(&pdev->dev);
		success = true;
		break;
		
		//igb_release_hw_control(adapter);
#ifdef HAVE_I2C_SUPPORT
		memset(&adapter->i2c_adap, 0, sizeof(adapter->i2c_adap));
#endif /* HAVE_I2C_SUPPORT */
	err_eeprom:
		if (!e1000_check_reset_block(hw))
			e1000_phy_hw_reset(hw);
		
	err_sw_init:
		kfree(adapter->shadow_vfta,sizeof(u32) * E1000_VFTA_ENTRIES);
		igb_clear_interrupt_scheme(adapter);
		igb_reset_sriov_capability(adapter);
		RELEASE(csrPCIAddress);	// iounmap(hw->io_addr);
	} while(false);
	
	
	return success;
}
		
bool AppleIGB::getBoolOption(const char *name, bool defVal)
{
	OSBoolean* rc = OSDynamicCast( OSBoolean, getProperty(name));
	if( rc ){
		return (rc == kOSBooleanTrue );
	}
	return defVal;
}
	
int AppleIGB::getIntOption(const char *name, int defVal, int maxVal, int minVal )
{
	int val = defVal;
	OSNumber* numObj = OSDynamicCast( OSNumber, getProperty(name) );
	if ( numObj ){
		val = (int)numObj->unsigned32BitValue();
		if( val < minVal )
			val = minVal;
		else if(val > maxVal )
			val = maxVal;
	}
	return val;
}
		
bool AppleIGB::start(IOService* provider)
{
    u32 i;

    #ifdef APPLE_OS_LOG
    igb_logger = os_log_create("com.amdosx.driver.AppleIGB", "Drivers");
    #endif

	pr_err("start()\n");
	
	if (super::start(provider) == false) {
		return false;
	}

	pdev = OSDynamicCast(IOPCIDevice, provider);
	if (pdev == NULL)
		return false;
	
	pdev->retain();
	if (pdev->open(this) == false)
		return false;

#ifdef NETIF_F_TSO
	useTSO = getBoolOption("NETIF_F_TSO", TRUE);
#else
	useTSO = FALSE;
#endif

    /** igb_probe requires watchdog to be intialized*/
    if(!initEventSources(provider)) {
        pr_err("Failed to initEventSources()\n");
        return false;
    }

    if(!igb_probe()) {
        pr_err("Failed to igb_probe()\n");
        return false;
    }

    if (!setupMediumDict()) {
        pr_err("Failed to setupMediumDict\n");
        return false;
    }

    chip_idx = 0;
    for( i = 0; i < sizeof(deviceModelNames)/sizeof(deviceModelNames[0]); i++){
        if(priv_adapter.hw.device_id == deviceModelNames[i].id )
            chip_idx = i;
    }

	// Close our provider, it will be re-opened on demand when
	// our enable() is called by a client.
	pdev->close(this);
	
	// Allocate and attach an IOEthernetInterface instance.
    if (attachInterface((IONetworkInterface**)&netif, false) == false) {
        pr_err("attachInterface() failed \n");
		return false;
    }

    netif->registerService();

    return true;

}

#ifdef HAVE_I2C_SUPPORT
/*
 *  igb_remove_i2c - Cleanup  I2C interface
 *  @adapter: pointer to adapter structure
 *
 */
static void igb_remove_i2c(struct igb_adapter *adapter)
{
	
	/* free the adapter bus structure */
	i2c_del_adapter(&adapter->i2c_adap);
}
#endif /* HAVE_I2C_SUPPORT */

//---------------------------------------------------------------------------
bool AppleIGB::initEventSources( IOService* provider )
{
    bool result = false;

    pr_debug("initEventSources() ===>\n");

	// Get a handle to our superclass' workloop.
	//
	IOWorkLoop* myWorkLoop = getWorkLoop();
	if (myWorkLoop == NULL) {
        if (!createWorkLoop()) {
            pr_err("No workloop and failed to create one\n");
            return false;
        }
        myWorkLoop = getWorkLoop();
		return false;
	}

	transmitQueue = getOutputQueue();
	if (transmitQueue == NULL) {
        pr_err("Unexpected transmitQueue\n");
		return false;
	}
    transmitQueue->retain();

#ifdef MSIX_ENABLED
    while (pdev->getInterruptType(intrIndex, &intrType) == kIOReturnSuccess) {
        if (intrType & kIOInterruptTypePCIMessaged){
            msiIndex = intrIndex;
            break;
        }
        intrIndex++;
    }

    if (msiIndex != -1) {
        pr_err("MSI interrupt index: %d\n", msiIndex);
        interruptSource = IOInterruptEventSource::interruptEventSource(this, OSMemberFunctionCast(IOInterruptEventSource::Action, this, &AppleIGB::interruptOccurred), provider, msiIndex);
    }
#endif

    interruptSource = IOInterruptEventSource::interruptEventSource(this,&AppleIGB::interruptHandler,provider);
    if (!interruptSource) {
        pr_err("MSI interrupt could not be enabled.\n");
        goto error1;
    }
    getWorkLoop()->addEventSource(interruptSource);

	watchdogSource = IOTimerEventSource::timerEventSource(this, &AppleIGB::watchdogHandler );
    if (!watchdogSource) {
        pr_err("Failed to create IOTimerEventSource.\n");
        goto error2;
    }
	getWorkLoop()->addEventSource(watchdogSource);

	resetSource = IOTimerEventSource::timerEventSource(this, &AppleIGB::resetHandler );
	getWorkLoop()->addEventSource(resetSource);

	dmaErrSource = IOTimerEventSource::timerEventSource(this, &AppleIGB::resetHandler );
	getWorkLoop()->addEventSource(dmaErrSource);

    pr_debug("initEventSources() <===\n");
	return true;
done:
    return result;

error2:
    workLoop->removeEventSource(interruptSource);
    RELEASE(interruptSource);

error1:
    pr_err("Error initializing event sources.\n");
    transmitQueue->release();
    transmitQueue = NULL;
    goto done;
}

//---------------------------------------------------------------------------
IOReturn AppleIGB::enable(IONetworkInterface * netif)
{
    const IONetworkMedium *selectedMedium;
    struct e1000_hw *hw = &priv_adapter.hw;
    int ret_val;
	pr_err("enable() ===>\n");
	if(!enabledForNetif){
		pdev->open(this);

        selectedMedium = getSelectedMedium();

        if (!selectedMedium) {
            pr_err("No medium selected. Falling back to autonegotiation.\n");
            selectedMedium = mediumTable[MEDIUM_INDEX_AUTO];
            setCurrentMedium(selectedMedium);
        }

        setCarrier(false);

        intelSetupAdvForMedium(selectedMedium);

        ret_val = igb_open(this);
        if (ret_val) {
            pr_err("igb_open failed %d\n", ret_val);
            return kIOReturnIOError;
        }

        // hack to accept any VLAN
        for(u16 k = 1; k < 4096; k++){
            igb_vlan_rx_add_vid(this,k);
        }

        interruptSource->enable();
        setTimers(true);

        if (!transmitQueue->setCapacity(IGB_DEFAULT_TXD)) {
            pr_err("Failed to set tx queue capacity %u\n", IGB_DEFAULT_TXD);
        }

        if (!carrier()) {
            setCarrier(true); // setValidLinkStatus(Active)
        }

        eeeMode = 0;
        stalled = FALSE;

        hw->mac.get_link_status = true;

		enabledForNetif = true;
    } else {
        pr_err("enabled already \n");
    }
    pr_err("enable() <===\n");
	return kIOReturnSuccess; 

}

IOReturn AppleIGB::disable(IONetworkInterface * netif)
{
    pr_err("disable() ===>\n");

	if(enabledForNetif){
		enabledForNetif = false;

        stopTxQueue();
        transmitQueue->setCapacity(0);

        watchdogSource->cancelTimeout();
        interruptSource->disable();
        setTimers(false);

		igb_close(this);

        igb_irq_disable(&priv_adapter);

        eeeMode = 0;
        stalled = FALSE;

        if (carrier()) {
            setCarrier(false);
            pr_debug("Link down on en%u\n", netif->getUnitNumber());
        }

        if (pdev && pdev->isOpen())
            pdev->close(this);
    } else {
        pr_err("disable() on not enabled interface\n");
    }

    pr_err("disable() <===\n");
	return kIOReturnSuccess;
}

static const char *speed1GName = "1-Gigabit";
static const char *speed100MName = "100-Megabit";
static const char *speed10MName = "10-Megabit";
static const char *duplexFullName = "Full-duplex";
static const char *duplexHalfName = "Half-duplex";

static const char *flowControlNames[kFlowControlTypeCount] = {
    "No flow-control",
    "Rx flow-control",
    "Tx flow-control",
    "Rx/Tx flow-control",
};

static const char* eeeNames[kEEETypeCount] = {
    "",
    ", energy-efficient-ethernet"
};

void AppleIGB::setLinkUp()
{
    struct e1000_hw *hw = &priv_adapter.hw;
    struct e1000_phy_info *phy = &hw->phy;
    struct igb_adapter *adapter = &priv_adapter;
    const char *flowName;
    const char *speedName;
    const char *duplexName;
    const char *eeeName;
    UInt64 mediumSpeed;
    UInt32 mediumIndex = MEDIUM_INDEX_AUTO;
    UInt32 fcIndex;
    UInt32 ctrl;

    pr_err("setLinkUp() ===>\n");

    eeeMode = 0;
    eeeName = eeeNames[kEEETypeNo];

    e1000_get_phy_info(hw);

    e1000_check_downshift_generic(hw);
    if (phy->speed_downgraded)
        pr_debug("Link Speed was downgraded by SmartSpeed\n");

    hw->mac.ops.get_link_up_info(hw, &adapter->link_speed, &adapter->link_duplex);

    /* Get link speed, duplex and flow-control mode. */
    ctrl = E1000_READ_REG(hw, E1000_CTRL) & (E1000_CTRL_RFCE | E1000_CTRL_TFCE);

    switch (ctrl) {
        case (E1000_CTRL_RFCE | E1000_CTRL_TFCE):
            fcIndex = kFlowControlTypeRxTx;
            break;
        case E1000_CTRL_RFCE:
            fcIndex = kFlowControlTypeRx;
            break;
        case E1000_CTRL_TFCE:
            fcIndex = kFlowControlTypeTx;
            break;
        default:
            fcIndex = kFlowControlTypeNone;
            break;
    }

    flowName = flowControlNames[fcIndex];

    if (priv_adapter.link_speed == SPEED_1000) {
        mediumSpeed = kSpeed1000MBit;
        speedName = speed1GName;
        duplexName = duplexFullName;

        eeeMode = intelSupportsEEE(adapter);

        if (fcIndex == kFlowControlTypeNone) {
            if (eeeMode) {
                mediumIndex = MEDIUM_INDEX_1000FDEEE;
                eeeName = eeeNames[kEEETypeYes];
            } else {
                mediumIndex = MEDIUM_INDEX_1000FD;
            }
        } else {
            if (eeeMode) {
                mediumIndex = MEDIUM_INDEX_1000FDFCEEE;
                eeeName = eeeNames[kEEETypeYes];
            } else {
                mediumIndex = MEDIUM_INDEX_1000FDFC;
            }
        }

    } else if (priv_adapter.link_speed == SPEED_100) {
       mediumSpeed = kSpeed100MBit;
       speedName = speed100MName;

       if (priv_adapter.link_duplex != DUPLEX_FULL) {
           duplexName = duplexFullName;

           eeeMode = intelSupportsEEE(adapter);

           if (fcIndex == kFlowControlTypeNone) {
               if (eeeMode) {
                   mediumIndex = MEDIUM_INDEX_100FDEEE;
                   eeeName = eeeNames[kEEETypeYes];
               } else {
                   mediumIndex = MEDIUM_INDEX_100FD;
               }
           } else {
               if (eeeMode) {
                   mediumIndex = MEDIUM_INDEX_100FDFCEEE;
                   eeeName = eeeNames[kEEETypeYes];
               } else {
                   mediumIndex = MEDIUM_INDEX_100FDFC;
               }
           }
       } else {
                mediumIndex = MEDIUM_INDEX_100HD;
                duplexName = duplexHalfName;
       }
   } else {
       mediumSpeed = kSpeed10MBit;
       speedName = speed10MName;

       if (priv_adapter.link_duplex != DUPLEX_FULL) {
           mediumIndex = MEDIUM_INDEX_10FD;
           duplexName = duplexFullName;
       } else {
           mediumIndex = MEDIUM_INDEX_10HD;
           duplexName = duplexHalfName;
       }
   }

    /* adjust timeout factor according to speed/duplex */
    adapter->tx_timeout_factor = 1;
    switch (adapter->link_speed) {
    case SPEED_10:
        adapter->tx_timeout_factor = 14;
        break;
    case SPEED_100:
        /* maybe add some timeout factor ? */
        break;
    default:
        break;
    }

    while (test_and_set_bit(__IGB_RESETTING, &adapter->state))
        usleep_range(1000, 2000);

    if (!carrier())
        setCarrier(true);

    igb_up(adapter);

    clear_bit(__IGB_RESETTING, &adapter->state);

    linkUp = true;

    if (stalled) {
        transmitQueue->service();
        stalled = false;
        pr_debug("Restart stalled queue!\n");
    }

    interruptSource->enable();
    setTimers(true);

    pr_debug("[LU]: Link Up on en%u (%s), %s, %s, %s%s\n",
             netif->getUnitNumber(), deviceModelNames[chip_idx].name,
             speedName, duplexName, flowName, eeeName);

    pr_debug("[LU]: CTRL=0x%08x\n", E1000_READ_REG(hw, E1000_CTRL));
    pr_debug("[LU]: CTRL_EXT=0x%08x\n", E1000_READ_REG(hw, E1000_CTRL_EXT));
    pr_debug("[LU]: STATUS=0x%08x\n", E1000_READ_REG(hw, E1000_STATUS));
    pr_debug("[LU]: RCTL=0x%08x\n", E1000_READ_REG(hw, E1000_RCTL));
    pr_debug("[LU]: PSRCTL=0x%08x\n", E1000_READ_REG(hw, E1000_PSRCTL));
    pr_debug("[LU]: FCRTL=0x%08x\n", E1000_READ_REG(hw, E1000_FCRTL));
    pr_debug("[LU]: FCRTH=0x%08x\n", E1000_READ_REG(hw, E1000_FCRTH));
    pr_debug("[LU]: RDLEN(0)=0x%08x\n", E1000_READ_REG(hw, E1000_RDLEN(0)));
    pr_debug("[LU]: RDTR=0x%08x\n", E1000_READ_REG(hw, E1000_RDTR));
    pr_debug("[LU]: RADV=0x%08x\n", E1000_READ_REG(hw, E1000_RADV));
    pr_debug("[LU]: RXCSUM=0x%08x\n", E1000_READ_REG(hw, E1000_RXCSUM));
    pr_debug("[LU]: RFCTL=0x%08x\n", E1000_READ_REG(hw, E1000_RFCTL));
    pr_debug("[LU]: RXDCTL(0)=0x%08x\n", E1000_READ_REG(hw, E1000_RXDCTL(0)));
    pr_debug("[LU]: RAL(0)=0x%08x\n", E1000_READ_REG(hw, E1000_RAL(0)));
    pr_debug("[LU]: RAH(0)=0x%08x\n", E1000_READ_REG(hw, E1000_RAH(0)));
    pr_debug("[LU]: MRQC=0x%08x\n", E1000_READ_REG(hw, E1000_MRQC));
    pr_debug("[LU]: TARC(0)=0x%08x\n", E1000_READ_REG(hw, E1000_TARC(0)));
    pr_debug("[LU]: TARC(1)=0x%08x\n", E1000_READ_REG(hw, E1000_TARC(1)));
    pr_debug("[LU]: TCTL=0x%08x\n", E1000_READ_REG(hw, E1000_TCTL));
    pr_debug("[LU]: TXDCTL(0)=0x%08x\n", E1000_READ_REG(hw, E1000_TXDCTL(0)));
    pr_debug("[LU]: TXDCTL(1)=0x%08x\n", E1000_READ_REG(hw, E1000_TXDCTL(1)));
    pr_debug("[LU]: EEE Active %u\n", (E1000_READ_REG(hw, E1000_EEER) & E1000_EEER_EEE_NEG));

    pr_err("setLinkUp() <===\n");
}

void AppleIGB::systemWillShutdown(IOOptionBits specifier)
{
    pr_debug("systemWillShutdown() ===>\n");

    if ((kIOMessageSystemWillPowerOff | kIOMessageSystemWillRestart) & specifier) {
        disable(netif);

        /* Restore the original MAC address. */
        priv_adapter.hw.mac.ops.rar_set(&priv_adapter.hw, priv_adapter.hw.mac.perm_addr, 0);

                /*
                 * Let the firmware know that the network interface is now closed
                 */
        igb_release_hw_control(&priv_adapter);
    }

    pr_debug("systemWillShutdown() <===\n");

    /* Must call super on shutdown or system will stall. */
    super::systemWillShutdown(specifier);
}

/** This method doesn't completely shutdown NIC. It intentionally keeps eventSources
 * and enables interruptes back
 */
void AppleIGB::setLinkDown()
{
        struct e1000_hw *hw = &priv_adapter.hw;
        struct igb_adapter *adapter = &priv_adapter;

        pr_err("setLinkDown() ===>\n");

        linkUp = false;
        /** igb_down also performs setLinkStatus(Valid) via netif_carrier_off */
        igb_down(adapter);

        clear_bit(__IGB_DOWN, &adapter->state);

        /* Clear any pending interrupts. */
        E1000_READ_REG(hw, E1000_ICR);
        igb_irq_enable(adapter);

        pr_err("Link down on en%u\n", netif->getUnitNumber());
        pr_err("setLinkDown() <===\n");
    }

// corresponds to igb_xmit_frame
UInt32 AppleIGB::outputPacket(mbuf_t skb, void * param)
{
	struct igb_adapter *adapter = &priv_adapter;
    UInt32 result = kIOReturnOutputDropped;
	
    if (unlikely(!(enabledForNetif && linkUp) || !txMbufCursor
            || test_bit(__IGB_DOWN, &adapter->state))) {
        pr_debug("output: Dropping packet on disabled device\n");
        goto error;
	}

	/*
	 * The minimum packet size with TCTL.PSP set is 17 so pad the skb
	 * in order to meet this minimum size requirement.
	 */
	// not applied to Mac OS X
	// igb_xmit_frame_ring is inlined here
	do {
        struct igb_ring *tx_ring = igb_tx_queue_mapping(adapter, skb);
        struct igb_tx_buffer *first;
        int tso = 0;
        u32 tx_flags = 0;
        u8 hdr_len = 0;
        /* need: 1 descriptor per page,
         *       + 2 desc gap to keep tail from touching head,
         *       + 1 desc for skb->data,
         *       + 1 desc for context descriptor,
         * otherwise try next time */
        txNumFreeDesc = igb_desc_unused(tx_ring);
        if (txNumFreeDesc < MAX_SKB_FRAGS + 3 //igb_maybe_stop_tx
            || stalled) /** even if we have free desc we should exit in stalled mode as queue is enabled by threadsafe interrupts -> native igb code (see igb_poll) */
        {
            /* this is a hard error */
			netStats->outputErrors += 1;
#ifdef DEBUG
            if (netStats->outputErrors % 100 == 0)
                pr_debug("output: Dropping packets (%u), free\n", netStats->outputErrors);
#endif
            /* We should normally return kIOReturnOutputStall but a lot of other parts of code
             * are not ready for this (kernel panic) further on mbuf_pkthdr_len
             * so just error as in some other drivers hoping that upper layers will be able to
             * handle this properly */
            result = kIOReturnOutputStall;
            stalled = true;
            goto done;
        }
        /* record the location of the first descriptor for this packet */
        first = &tx_ring->tx_buffer_info[tx_ring->next_to_use];
        first->skb = skb;
        first->bytecount = (u32)mbuf_pkthdr_len(skb);
        first->gso_segs = 1;

#ifdef HAVE_PTP_1588_CLOCK
        if (unlikely(skb_shinfo(skb)->tx_flags & SKBTX_HW_TSTAMP)) {
            struct igb_adapter *adapter = netdev_priv(tx_ring->netdev);
            if (!adapter->ptp_tx_skb) {
                skb_shinfo(skb)->tx_flags |= SKBTX_IN_PROGRESS;
                tx_flags |= IGB_TX_FLAGS_TSTAMP;
                
                adapter->ptp_tx_skb = skb_get(skb);
                adapter->ptp_tx_start = jiffies;
                if (adapter->hw.mac.type == e1000_82576)
                    schedule_work(&adapter->ptp_tx_work);
            }
        }
#endif /* HAVE_PTP_1588_CLOCK */
        
        UInt32 vlan;
        if(getVlanTagDemand(skb,&vlan)){
			//pr_err("vlan(out) = %d\n",(int)vlan);
            tx_flags |= IGB_TX_FLAGS_VLAN;
            tx_flags |= (vlan << IGB_TX_FLAGS_VLAN_SHIFT);
        }
        
        /* record initial flags and protocol */
        first->tx_flags = tx_flags;
        
		if(useTSO)
			tso = igb_tso(tx_ring, first, &hdr_len);
        if (unlikely(tso < 0)){
            igb_unmap_and_free_tx_resource(tx_ring, first);
            break;
        } else if (!tso)
            igb_tx_csum(tx_ring, first);

        if(!igb_tx_map(tx_ring, first, hdr_len)){
			netStats->outputErrors += 1;
            pr_debug("output: igb_tx_map failed (%u)\n", netStats->outputErrors);
            goto error;
		}

		/* Make sure there is space in the ring for the next send. */
		//igb_maybe_stop_tx(tx_ring, MAX_SKB_FRAGS + 4);
    } while(false);

    result = kIOReturnOutputSuccess;

done:
    //DebugLog("[IntelMausi]: outputPacket() <===\n");
    return result;

error:
    freePacket(skb);
    goto done;
}

void AppleIGB::getPacketBufferConstraints(IOPacketBufferConstraints * constraints) const
{
	constraints->alignStart = kIOPacketBufferAlign2;
	constraints->alignLength = kIOPacketBufferAlign1;
	return;
}

IOOutputQueue * AppleIGB::createOutputQueue()
{
	return IOGatedOutputQueue::withTarget(this, getWorkLoop());
}

const OSString * AppleIGB::newVendorString() const
{
	return OSString::withCString("Intel");
}

const OSString * AppleIGB::newModelString() const
{
    if (chip_idx)
        return OSString::withCString(deviceModelNames[chip_idx].name);

	return OSString::withCString("Unknown");
}

#define ADVERTISED_10baseT_Half        (1 << 0)
#define ADVERTISED_10baseT_Full        (1 << 1)
#define ADVERTISED_100baseT_Half    (1 << 2)
#define ADVERTISED_100baseT_Full    (1 << 3)
#define ADVERTISED_1000baseT_Half    (1 << 4)
#define ADVERTISED_1000baseT_Full    (1 << 5)
#define ADVERTISED_Autoneg        (1 << 6)
#define ADVERTISED_TP            (1 << 7)
#define ADVERTISED_AUI            (1 << 8)
#define ADVERTISED_MII            (1 << 9)
#define ADVERTISED_FIBRE        (1 << 10)
#define ADVERTISED_BNC            (1 << 11)
#define ADVERTISED_10000baseT_Full    (1 << 12)
#define ADVERTISED_Pause        (1 << 13)
#define ADVERTISED_Asym_Pause        (1 << 14)
#define ADVERTISED_2500baseX_Full    (1 << 15)
#define ADVERTISED_Backplane        (1 << 16)
#define ADVERTISED_1000baseKX_Full    (1 << 17)

/**
* intelSupportsEEE
*/
UInt16 AppleIGB::intelSupportsEEE(struct igb_adapter *adapter)
{
    struct e1000_hw *hw = &adapter->hw;
    struct e1000_mac_info *mac = &hw->mac;

    UInt16 result = 0;

    if ((mac->type < e1000_i350) || (hw->phy.media_type != e1000_media_type_copper))
        goto done;

    if (hw->dev_spec._82575.eee_disable)
        goto done;

    result |= E1000_EEE_ADV_100_SUPPORTED | E1000_EEE_ADV_1000_SUPPORTED;

    return result;

done:
    return result;
}

/**
* intelSetupAdvForMedium @IntelMausi
*/
void AppleIGB::intelSetupAdvForMedium(const IONetworkMedium *medium)
{
        struct igb_adapter *adapter = &priv_adapter;
        struct e1000_hw *hw = &adapter->hw;
        struct e1000_mac_info *mac = &hw->mac;
        IOMediumType type = medium->getType();

        pr_debug("intelSetupAdvForMedium(index %u, type %u) ===>\n", medium->getIndex(), type);

        /* SerDes device's does not support 10Mbps Full/duplex
         * and 100Mbps Half duplex
         * @see igb_set_spd_dplx
         */
        if (type != kIOMediumEthernetAuto
            && hw->phy.media_type == e1000_media_type_internal_serdes) {
                switch (type) {
                    case kIOMediumEthernet10BaseT | kIOMediumOptionHalfDuplex:
                    case kIOMediumEthernet10BaseT | kIOMediumOptionFullDuplex:
                    case kIOMediumEthernet100BaseTX | kIOMediumOptionHalfDuplex:
                        pr_err("Unsupported Speed/Duplex configuration\n");
                        /** @todo we probably shouldn't add these options to the dict */
                        return;
                    default:
                        break;
            }
        }

        hw->mac.autoneg = 0;

        if (intelSupportsEEE(adapter))
            hw->dev_spec._82575.eee_disable = true;

        switch (medium->getIndex()) {
            case MEDIUM_INDEX_10HD:
                mac->forced_speed_duplex = ADVERTISE_10_HALF;
                break;

            case MEDIUM_INDEX_10FD:
                mac->forced_speed_duplex = ADVERTISE_10_FULL;
                break;

            case MEDIUM_INDEX_100HD:
                hw->mac.forced_speed_duplex = ADVERTISE_100_HALF;
                hw->fc.requested_mode = e1000_fc_none;
                break;

            case MEDIUM_INDEX_100FD:
                hw->mac.forced_speed_duplex = ADVERTISE_100_FULL;
                hw->fc.requested_mode = e1000_fc_none;
                break;

            case MEDIUM_INDEX_100FDFC:
                hw->mac.forced_speed_duplex = ADVERTISE_100_FULL;
                hw->fc.requested_mode = e1000_fc_full;
                break;

            case MEDIUM_INDEX_1000FD:
                hw->phy.autoneg_advertised = ADVERTISE_1000_FULL;
                hw->mac.autoneg = 1;
                hw->fc.requested_mode = e1000_fc_none;
                break;

            case MEDIUM_INDEX_1000FDFC:
                hw->phy.autoneg_advertised = ADVERTISE_1000_FULL;
                hw->mac.autoneg = 1;
                hw->fc.requested_mode = e1000_fc_full;
                break;

            case MEDIUM_INDEX_1000FDEEE:
                hw->phy.autoneg_advertised = ADVERTISE_1000_FULL;
                hw->mac.autoneg = 1;
                hw->fc.requested_mode = e1000_fc_none;
                hw->dev_spec._82575.eee_disable = false;
                break;

            case MEDIUM_INDEX_1000FDFCEEE:
                hw->phy.autoneg_advertised = ADVERTISE_1000_FULL;
                hw->mac.autoneg = 1;
                hw->fc.requested_mode = e1000_fc_full;
                hw->dev_spec._82575.eee_disable = false;
                break;

            case MEDIUM_INDEX_100FDEEE:
                hw->phy.autoneg_advertised = ADVERTISE_100_FULL;
                hw->mac.autoneg = 1;
                hw->fc.requested_mode = e1000_fc_none;
                hw->dev_spec._82575.eee_disable = false;
                break;

            case MEDIUM_INDEX_100FDFCEEE:
                hw->phy.autoneg_advertised = ADVERTISE_100_FULL;
                hw->mac.autoneg = 1;
                hw->fc.requested_mode = e1000_fc_full;
                hw->dev_spec._82575.eee_disable = false;
                break;

            default:
                if (hw->phy.media_type == e1000_media_type_fiber) {
                    /** @see igb_set_link_ksettings **/
                    hw->phy.autoneg_advertised = ADVERTISED_1000baseT_Full
                        | ADVERTISED_FIBRE | ADVERTISED_Autoneg;
                    switch (adapter->link_speed) {
                        case SPEED_2500:
                            hw->phy.autoneg_advertised = ADVERTISED_2500baseX_Full;
                            break;
                        case SPEED_1000:
                            hw->phy.autoneg_advertised = ADVERTISED_1000baseT_Full;
                            break;
                        case SPEED_100:
                            hw->phy.autoneg_advertised = ADVERTISED_100baseT_Full;
                            break;
                        default:
                            pr_err("Unexpected link_speed\n");
                            break;
                    }
                } else {
                    hw->phy.autoneg_advertised = E1000_ALL_SPEED_DUPLEX
                        | ADVERTISED_TP | ADVERTISED_Autoneg;
                }
                if (adapter->fc_autoneg)
                        hw->fc.requested_mode = e1000_fc_default;

                if (intelSupportsEEE(adapter))
                    hw->dev_spec._82575.eee_disable = false;

                hw->mac.autoneg = 1;
                break;
        }
        /* clear MDI, MDI(-X) override is only allowed when autoneg enabled */
        hw->phy.mdix = AUTO_ALL_MODES;

        pr_debug("intelSetupAdvForMedium() <===\n");
}

        /**
         * intelRestart
         *
         * Reset the NIC in case a tx deadlock or a pci error occurred. timerSource and txQueue
         * are stopped immediately but will be restarted by checkLinkStatus() when the link has
         * been reestablished.
         *
         * From IntelMausi
         */
void AppleIGB::intelRestart() {
        struct igb_adapter *adapter = &priv_adapter;

        pr_debug("intelRestart ===> on en%u, linkUp=%u, carrier=%u\n",
                 netif->getUnitNumber(), linkUp, carrier());

        linkUp = false;
        eeeMode = 0;

        while (test_and_set_bit(__IGB_RESETTING, &adapter->state))
            usleep_range(1000, 2000);

        if (netif_running(this)) {
            /**
             * igb_down and igb_up do everything IntelMausi performs in its version:
             *  - netif_carrier_off = setLinkStatus(valid)
             *  - stop transmit queues
             *  - disable IRQ
             *  - reset HW
             *  - configure
             *  - enable IRQ
             *  - start transmit queues
             * So no obvious reason to avoid reusing as is.
             */
            pr_debug("igb_down...\n");
            igb_down(adapter);
            pr_debug("igb_up...\n");
            igb_up(adapter);
        } else {
            pr_debug("igb_reset...\n");
            igb_reset(adapter);
        }

        clear_bit(__IGB_RESETTING, &adapter->state);
        pr_debug("intelRestart <===\n");
}

IOReturn AppleIGB::selectMedium(const IONetworkMedium * medium)
{
    pr_err("selectMedium()===>\n");

    if (medium) {
        intelSetupAdvForMedium(medium);
        setCurrentMedium(medium);

        igb_update_stats(&priv_adapter);

        intelRestart();
    } else {
        pr_err("Unexpected medium, ignoring.\n");
    }
    pr_err("<===selectMedium()\n");
	return kIOReturnSuccess;
}

#define kNameLength 60
bool AppleIGB::configureInterface(IONetworkInterface * interface)
{
    char modelName[kNameLength];
	IONetworkData * data = NULL;
	
	if (super::configureInterface(interface) == false) {
		pr_err("IOEthernetController::confiugureInterface failed.\n");
		return false;
	}
	
	// Get the generic network statistics structure.
	data = interface->getParameter(kIONetworkStatsKey);
	if (!data || !(netStats = (IONetworkStats *) data->getBuffer())) {
		pr_err("netif getParameter NetworkStatsKey failed.\n");
		return false;
	}
	
	// Get the Ethernet statistics structure.
	data = interface->getParameter(kIOEthernetStatsKey);
	if (!data || !(etherStats = (IOEthernetStats *) data->getBuffer())) {
		pr_err("netif getParameter kIOEthernetStatsKey failed.\n");
		return false;
	}

    snprintf(modelName, kNameLength, "Intel(R) Ethernet Controller %s (IGB)", deviceModelNames[chip_idx].name);
    setProperty("model", modelName);

	return true;
}

bool AppleIGB::createWorkLoop()
{
    if ((vm_address_t) workLoop >> 1)
     return true;

    if (OSCompareAndSwap(0, 1, (UInt32 *) &workLoop)) {
        // Construct the workloop and set the cntrlSync variable
        // to whatever the result is and return
        workLoop = IOWorkLoop::workLoop();
    } else while ((IOWorkLoop *) workLoop == (IOWorkLoop *) 1)
        // Spin around the cntrlSync variable until the
        // initialization finishes.
        thread_block(0);
    return workLoop != NULL;
}

IOWorkLoop * AppleIGB::getWorkLoop() const
{
   return workLoop;
}

//-----------------------------------------------------------------------
// Methods inherited from IOEthernetController.
//-----------------------------------------------------------------------

IOReturn AppleIGB::getHardwareAddress(IOEthernetAddress * addr)
{
	memcpy(addr->bytes, priv_adapter.hw.mac.addr, kIOEthernetAddressSize);
	return kIOReturnSuccess;
}

// corresponds to igb_set_mac
IOReturn AppleIGB::setHardwareAddress(const IOEthernetAddress * addr)
{
	igb_adapter *adapter = &priv_adapter;
	struct e1000_hw *hw = &adapter->hw;

	

	
	igb_del_mac_filter(adapter, hw->mac.addr,
					   adapter->vfs_allocated_count);
	memcpy(adapter->hw.mac.addr, addr->bytes, kIOEthernetAddressSize);
	
	/* set the correct pool for the new PF MAC address in entry 0 */
	igb_add_mac_filter(adapter, hw->mac.addr,
							  adapter->vfs_allocated_count);
	
	return kIOReturnSuccess;
}
IOReturn AppleIGB::setPromiscuousMode(bool active)
{
	if(active)
		iff_flags |= IFF_PROMISC;
	else
		iff_flags &= ~IFF_PROMISC;

	igb_set_rx_mode(this);
	return kIOReturnSuccess;
}
	
IOReturn AppleIGB::setMulticastMode(bool active)
{
	if(active)
		iff_flags |= IFF_ALLMULTI;
	else
		iff_flags &= IFF_ALLMULTI;
	
	igb_set_rx_mode(this);
	return kIOReturnSuccess;
}

// corresponds to igb_write_mc_addr_list	
IOReturn AppleIGB::setMulticastList(IOEthernetAddress * addrs, UInt32 count)
{
	igb_adapter *adapter = &priv_adapter;
	struct e1000_hw *hw = &adapter->hw;

	if (!count) {
		e1000_update_mc_addr_list(hw, NULL, 0);
		return 0;
	}
	
	/* The shared function expects a packed array of only addresses. */
	e1000_update_mc_addr_list(hw, (u8*)addrs, count);
	
	return kIOReturnSuccess;
}

IOReturn AppleIGB::getChecksumSupport(UInt32 *checksumMask, UInt32 checksumFamily, bool isOutput) 
{
	*checksumMask = 0;
	if( checksumFamily != kChecksumFamilyTCPIP ) {
		pr_err("AppleIGB: Operating system wants information for unknown checksum family.\n");
		return kIOReturnUnsupported;
	}
	/*
	 * kChecksumTCPIPv6 = 0x0020,
	 * kChecksumUDPIPv6 = 0x0040,
	 */
	if( !isOutput ) {
		*checksumMask = kChecksumTCP | kChecksumUDP | kChecksumIP | CSUM_TCPIPv6 | CSUM_UDPIPv6;
	} else {
#if USE_HW_UDPCSUM
		*checksumMask = kChecksumTCP | kChecksumUDP | CSUM_TCPIPv6 | CSUM_UDPIPv6;
#else
		*checksumMask = kChecksumTCP | CSUM_TCPIPv6;
#endif
	}
    return kIOReturnSuccess;
}

//-----------------------------------------------------------------------
// e1000e private functions
//-----------------------------------------------------------------------
bool AppleIGB::addNetworkMedium(UInt32 type, UInt32 bps, UInt32 index)
{
	IONetworkMedium *medium;
	
	medium = IONetworkMedium::medium(type, bps, 0, index);
	if (!medium) {
		pr_err("Couldn't allocate medium.\n");
		return false;
	}
	
	if (!IONetworkMedium::addMedium(mediumDict, medium)) {
		pr_err("Couldn't add medium.\n");
        RELEASE(medium);
		return false;
	}
	
	mediumTable[index] = medium;
	medium->release();
	return true;
}

/**
* intelCheckLink
* It's not exact copy of igb_has_link, additional check for E1000_STATUS_LU (Link up)
* is performed
* Reference: igb_has_link
*/
bool AppleIGB::intelCheckLink(struct igb_adapter *adapter)
{
    struct e1000_hw *hw = &adapter->hw;
    bool link_active = FALSE;
    s32 ret_val = 0, status;

    /* get_link_status is set on LSC (link status) interrupt or
     * rx sequence error interrupt.  get_link_status will stay
     * false until the e1000_check_for_link establishes link
     * for copper adapters ONLY
     */
    switch (hw->phy.media_type) {
    case e1000_media_type_copper:
        if (!hw->mac.get_link_status)
            return true;
        /* Fall through */
    case e1000_media_type_internal_serdes:
        /** on I211 this effectively calls e1000_check_for_copper_link_generic() */
        ret_val = e1000_check_for_link(hw);
        link_active = !hw->mac.get_link_status;
        if (!link_active) {
            /**It seems MII_SR_LINK_STATUS register might not be set
             * if setLinkStatus(Active) hasn't been called before.
             * So checking E1000_STATUS_LU (Link Up) additionally (per IntelMausi).
             */
            status = E1000_READ_REG(hw, E1000_STATUS);
            link_active = !!(status & E1000_STATUS_LU);
            pr_debug("E1000_STATUS_LU=%u (0x%08x)\n", link_active, status);
        }
        break;
    case e1000_media_type_unknown:
    default:
        pr_debug("Unknown media type\n");
        break;
    }

    if (((hw->mac.type == e1000_i210) ||
         (hw->mac.type == e1000_i211)) &&
         (hw->phy.id == I210_I_PHY_ID)) {
        if (!netif_carrier_ok(adapter->netdev)) {
            adapter->flags &= ~IGB_FLAG_NEED_LINK_UPDATE;
        } else if (!(adapter->flags & IGB_FLAG_NEED_LINK_UPDATE)) {
            adapter->flags |= IGB_FLAG_NEED_LINK_UPDATE;
            adapter->link_check_timeout = jiffies;
        }
    }

    return link_active;
}

/**
  this is called by interrupt
    @see igb_watchdog_task
 */
void AppleIGB::checkLinkStatus()
{
    struct igb_adapter *adapter = &priv_adapter;
    struct e1000_hw *hw = &priv_adapter.hw;
    u32 link;
    u32 connsw;

    hw->mac.get_link_status = true;

    /* Now check the link state. */
    link = intelCheckLink(adapter);

    pr_debug("checkLinkStatus() ===> link=%u, carrier=%u, linkUp=%u\n",
             link, carrier(), linkUp);

    /* Force link down if we have fiber to swap to */
    if (adapter->flags & IGB_FLAG_MAS_ENABLE) {
        if (hw->phy.media_type == e1000_media_type_copper) {
            connsw = E1000_READ_REG(hw, E1000_CONNSW);
            if (!(connsw & E1000_CONNSW_AUTOSENSE_EN)) {
                pr_debug("Force link down if we have fiber to swap to\n");
                link = 0;
            }

        }
    }
    if (adapter->flags & IGB_FLAG_NEED_LINK_UPDATE) {
        if (time_after(jiffies, (adapter->link_check_timeout + HZ)))
            adapter->flags &= ~IGB_FLAG_NEED_LINK_UPDATE;
        else {
            pr_debug("Force link down due to IGB_FLAG_NEED_LINK_UPDATE\n");
            link = FALSE;
        }
    }

    if (link) {
        /* Perform a reset if the media type changed. */
        if (hw->dev_spec._82575.media_changed) {
            hw->dev_spec._82575.media_changed = false;
            adapter->flags |= IGB_FLAG_MEDIA_RESET;
            pr_debug("Perform a reset if the media type changed.\n");
            igb_reset(adapter);
        }
    }

    if (linkUp)
    {
        if (link) {
            /* The link partner must have changed some setting. Initiate renegotiation
             * of the link parameters to make sure that the MAC is programmed correctly.
             */
            watchdogSource->cancelTimeout();
            igb_update_stats(&priv_adapter);
            intelRestart();
        } else {
            /* Stop watchdog and statistics updates. */
            watchdogSource->cancelTimeout();
            setLinkDown();
        }
    } else {
        if (link) {
            /* Start rx/tx and inform upper layers that the link is up now. */
            setLinkUp();
            /* Perform live checks periodically. */
            watchdogSource->setTimeoutMS(200);
       }
    }
    pr_debug("checkLinkStatus() <===\n");
}

// corresponds to igb-intr
void AppleIGB::interruptOccurred(IOInterruptEventSource * src, int count)
{
	struct igb_adapter *adapter = &priv_adapter;
	struct igb_q_vector *q_vector = adapter->q_vector[0];
	struct e1000_hw *hw = &adapter->hw;

    /* Interrupt Auto-Mask...upon reading ICR, interrupts are masked.  No
         * need for the IMC write */
    u32 icr = E1000_READ_REG(hw, E1000_ICR);

    if(!enabledForNetif) {
        pr_debug("Interrupt 0x%08x on disabled device\n", icr);
        return;
    }

	/* IMS will not auto-mask if INT_ASSERTED is not set, and if it is
	 * not set, then the adapter didn't send an interrupt */
	if (!(icr & E1000_ICR_INT_ASSERTED))
		return;
	
	igb_write_itr(q_vector);
	
    if (icr & E1000_ICR_DRSTA) {
        resetSource->setTimeoutMS(1);
    }

	if (icr & E1000_ICR_DOUTSYNC) {
		/* HW is reporting DMA is out of sync */
		adapter->stats.doosync++;
	}
	
	if (unlikely(icr & (E1000_ICR_RXSEQ | E1000_ICR_LSC))) {
        checkLinkStatus();
//
//		/* guard against interrupt when we're going down */
//		if (!test_bit(__IGB_DOWN, &adapter->state))
//			watchdogSource->setTimeoutMS(1);
    } else {
        igb_poll(q_vector, 64);
    }
}

void AppleIGB::interruptHandler(OSObject * target, IOInterruptEventSource * src, int count)
{
	AppleIGB * me = (AppleIGB *) target;
	me->interruptOccurred(src, count);
}


// corresponds to igb_watchdog_task	
void AppleIGB::watchdogTask()
{
	struct igb_adapter *adapter = &priv_adapter;
	struct e1000_hw *hw = &adapter->hw;
	int i;

	igb_update_stats(adapter);

    for (i = 0; i < adapter->num_tx_queues; i++) {
        struct igb_ring *tx_ring = adapter->tx_ring[i];

        /* Force detection of hung controller every watchdog period */
        set_bit(IGB_RING_FLAG_TX_DETECT_HANG, &tx_ring->flags);
    }

    /* Cause software interrupt to ensure rx ring is cleaned */
    if (adapter->msix_entries) {
        u32 eics = 0;

        for (i = 0; i < adapter->num_q_vectors; i++)
            eics |= adapter->q_vector[i]->eims_value;
        E1000_WRITE_REG(hw, E1000_EICS, eics);
    } else {
        E1000_WRITE_REG(hw, E1000_ICS, E1000_ICS_RXDMT0);
    }

	/* Reset the timer */
	if (!test_bit(__IGB_DOWN, &adapter->state)){
        if (adapter->flags & IGB_FLAG_NEED_LINK_UPDATE) {
            pr_debug("watchdogTask(): adapter has IGB_FLAG_NEED_LINK_UPDATE, forcing restart.\n");
            intelRestart();
        }
	}

    watchdogSource->setTimeoutMS(200);
}

// corresponds to igb_update_phy_info
void AppleIGB::updatePhyInfoTask()
{
    struct e1000_hw *hw = &priv_adapter.hw;

    e1000_get_phy_info(hw);
}
	
void AppleIGB::watchdogHandler(OSObject * target, IOTimerEventSource * src)
{
	AppleIGB* me = (AppleIGB*) target;
	me->watchdogTask();
	me->watchdogSource->setTimeoutMS(1000);
}
	
void AppleIGB::resetHandler(OSObject * target, IOTimerEventSource * src)
{
	AppleIGB* me = (AppleIGB*) target;
    if(src == me->resetSource) {
        pr_debug("resetHandler: resetSource\n");
		igb_reinit_locked(&me->priv_adapter);
    }
    else if(src == me->dmaErrSource) {
        pr_debug("resetHandler: dmaErrSource\n");
		igb_dma_err_task(&me->priv_adapter,src);
    }
}


IOReturn AppleIGB::registerWithPolicyMaker ( IOService * policyMaker )
{
	static IOPMPowerState powerStateArray[ 2 ] = {
		{ 1,0,0,0,0,0,0,0,0,0,0,0 },
		{ 1,kIOPMDeviceUsable,kIOPMPowerOn,kIOPMPowerOn,0,0,0,0,0,0,0,0 }
	};
	powerState = 1;
	return policyMaker->registerPowerDriver( this, powerStateArray, 2 );
}

IOReturn AppleIGB::setPowerState( unsigned long powerStateOrdinal,
								IOService *policyMaker )
{
	pr_err("setPowerState(%d)\n",(int)powerStateOrdinal);
	if (powerState == powerStateOrdinal)
		return IOPMAckImplied;
	powerState = powerStateOrdinal;

	if(powerStateOrdinal == 0){ // SUSPEND/SHUTDOWN
		pr_err("suspend start.\n");
		// ???
		pr_err("suspend end.\n");
		bSuspended = TRUE;
	} else if(bSuspended) { // WAKE
		pr_err("resume start.\n");
		// ???
		pr_err("resume end.\n");
		bSuspended = FALSE;
	}
	/* acknowledge the completion of our power state change */
    return IOPMAckImplied;
}

#define MAX_STD_JUMBO_FRAME_SIZE 9238
IOReturn AppleIGB::getMaxPacketSize (UInt32 *maxSize) const {
	if (maxSize)
		*maxSize = MAX_STD_JUMBO_FRAME_SIZE;  // or mtu = 9216 ?

	return kIOReturnSuccess;
}

IOReturn AppleIGB::getMinPacketSize (UInt32 *minSize) const {
	if(minSize)
		*minSize = ETH_ZLEN + ETH_FCS_LEN + VLAN_HLEN;
	
	return kIOReturnSuccess;
}


IOReturn AppleIGB::setMaxPacketSize (UInt32 maxSize){
	pr_err("AppleIGB::setMaxPacketSize(%d)\n",(int)maxSize);
	UInt32 newMtu = maxSize  - (ETH_HLEN + ETH_FCS_LEN);
	if(newMtu != _mtu){
        _mtu = newMtu;
        igb_change_mtu(this,_mtu);
	}
	return kIOReturnSuccess;
}

IOReturn AppleIGB::setWakeOnMagicPacket(bool active)
{
	igb_adapter *adapter = &priv_adapter;
	if(active){
       if ((adapter->flags & IGB_FLAG_WOL_SUPPORTED) == 0)
           return kIOReturnUnsupported;   
		adapter->wol = 1;
	} else {
		adapter->wol = 0;
	}
	return kIOReturnSuccess;   
}

IOReturn AppleIGB::getPacketFilters(const OSSymbol * group, UInt32 * filters) const {
	if(group == gIOEthernetWakeOnLANFilterGroup){
		*filters = kIOEthernetWakeOnMagicPacket;
		return kIOReturnSuccess;   
	}
#if defined(MAC_OS_X_VERSION_10_6)
	if(group == gIOEthernetDisabledWakeOnLANFilterGroup){
		*filters = 0;
		return kIOReturnSuccess;   
	}
#endif
	return super::getPacketFilters(group, filters);
}

UInt32 AppleIGB::getFeatures() const {
	UInt32 f = kIONetworkFeatureMultiPages | kIONetworkFeatureHardwareVlan;
	if(useTSO)
#ifdef NETIF_F_TSO6
		f |= kIONetworkFeatureTSOIPv4 | kIONetworkFeatureTSOIPv6;
#else
		f |= kIONetworkFeatureTSOIPv4;
#endif
	return f;
}
    
    

/**
 * Linux porting helpers
 **/


void AppleIGB::startTxQueue()
{
    DEBUGFUNC("AppleIGB::startTxQueue\n");
    if (likely(stalled && txMbufCursor && transmitQueue)) {
        pr_debug("Assuming wake queue called.\n");
        transmitQueue->service(IOBasicOutputQueue::kServiceAsync);
    } else {
        txMbufCursor = IOMbufNaturalMemoryCursor::withSpecification(_mtu + ETH_HLEN + ETH_FCS_LEN + VLAN_HLEN, MAX_SKB_FRAGS);
        if(txMbufCursor && transmitQueue)
            transmitQueue->start();
    }
    stalled = false;
}

void AppleIGB::stopTxQueue()
{
    pr_debug("AppleIGB::stopTxQueue()\n");
	transmitQueue->stop();
	transmitQueue->flush();
	RELEASE(txMbufCursor);
}

void AppleIGB::rxChecksumOK( mbuf_t skb, UInt32 flag )
{
	setChecksumResult(skb, kChecksumFamilyTCPIP, flag, flag );
}
	
bool AppleIGB::carrier()
{
	return (preLinkStatus & kIONetworkLinkActive) != 0;

}
	
void AppleIGB::setCarrier(bool stat)
{
    pr_debug("setCarrier(%d) ===>\n", stat);
	if(stat){
		preLinkStatus = kIONetworkLinkValid | kIONetworkLinkActive;
        /** @todo the device assumed to take the speed from the medium.
         * no reason, I _guess_ to force it here especially with 100mb max */
//		UInt64 speed = 1000 * MBit;
//		switch (priv_adapter.link_speed) {
//			case SPEED_10:
//				speed = 10 * MBit;
//				break;
//			case SPEED_100:
//				speed = 100 * MBit;
//				break;
//		}
        if (!setLinkStatus(preLinkStatus, getCurrentMedium())) {
            pr_err("setLinkStatus: Some properties were not updated successullly with current medium(%u)\n",
                   preLinkStatus);
        }
	} else {
		preLinkStatus = kIONetworkLinkValid;
        if (!setLinkStatus(preLinkStatus)) {
            pr_err("setLinkStatus(kIONetworkLinkValid): Some properties were not updated\n");
        }
	}

    pr_debug("setCarrier() <===\n");
}
	
void AppleIGB::receive(mbuf_t skb)
{
	netif->inputPacket(skb, mbuf_pkthdr_len(skb));
}

void AppleIGB::setVid(mbuf_t skb, UInt16 vid)
{
	setVlanTag(skb, vid);
}

void AppleIGB::setTimers(bool enable)
{
    if(enable){
        if(watchdogSource)
            watchdogSource->enable();
        if(resetSource)
            resetSource->enable();
        if(dmaErrSource)
            dmaErrSource->enable();
    } else {
        if(watchdogSource)
            watchdogSource->disable();
        if(resetSource)
            resetSource->disable();
        if(dmaErrSource)
            dmaErrSource->disable();
    }
}
   
