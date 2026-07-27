#include "kvm/kvm.h"
#include "kvm/vfio.h"

/*
 * --vfio-iommufd selects this backend on RISC-V to request second-stage
 * DMA translation. The type1 backend remains the default for ordinary
 * device assignment, which can also use first-stage IOMMU mappings.
 */

#include <linux/iommufd.h>
#include <linux/list.h>

#include <dirent.h>
#include <libgen.h>

#define IOMMU_DEV_NODE		"/dev/iommu"
#define VFIO_DEV_DIR		"/dev/vfio/devices"

static int iommufd_ctx = -1;
static u32 iommufd_ioas_id;
static struct vfio_device *iommufd_devices;
static int iommufd_num_devices;
static int iommufd_num_mapped_banks;

struct vfio_iommufd_group {
	unsigned long			id;
	int				refs;
	int				hwpt_users;
	u32				hwpt_id;
	struct vfio_device		*hwpt_vdev;
	struct list_head		list;
};

static LIST_HEAD(iommufd_groups);

static void vfio_iommufd_group_destroy_hwpt(struct vfio_iommufd_group *group)
{
	struct iommu_destroy destroy = {
		.size	= sizeof(destroy),
		.id	= group->hwpt_id,
	};

	if (!group->hwpt_id)
		return;

	if (ioctl(iommufd_ctx, IOMMU_DESTROY, &destroy))
		vfio_dev_warn(group->hwpt_vdev, "failed to destroy HWPT: %s",
			      strerror(errno));

	group->hwpt_id = 0;
	group->hwpt_vdev = NULL;
}

static int vfio_iommufd_container_init(struct kvm *kvm)
{
	struct iommu_ioas_alloc ioas_alloc = {
		.size = sizeof(ioas_alloc),
	};

	iommufd_ctx = open(IOMMU_DEV_NODE, O_RDWR);
	if (iommufd_ctx < 0) {
		int ret = -errno;

		pr_err("Failed to open %s: %s", IOMMU_DEV_NODE, strerror(errno));
		return ret;
	}

	if (ioctl(iommufd_ctx, IOMMU_IOAS_ALLOC, &ioas_alloc)) {
		int ret = -errno;

		pr_err("Failed to allocate IOAS: %s", strerror(errno));
		close(iommufd_ctx);
		iommufd_ctx = -1;
		return ret;
	}

	iommufd_ioas_id = ioas_alloc.out_ioas_id;

	return 0;
}

/*
 * A device's cdev is named by a "vfioN" sysfs entry under the device's
 * own "vfio-dev" directory rather than a fixed/predictable path, so it
 * has to be discovered by directory scan. See
 * Documentation/driver-api/vfio.rst's "VFIO device cdev" section.
 */
static int vfio_iommufd_get_cdev_path(struct vfio_device *vdev, char *path,
				      size_t path_sz)
{
	char dir_path[PATH_MAX];
	struct dirent *entry;
	int ret = -ENOENT;
	DIR *dir;

	if (snprintf(dir_path, PATH_MAX, "%s/vfio-dev", vdev->sysfs_path) >=
	    PATH_MAX)
		return -EINVAL;

	dir = opendir(dir_path);
	if (!dir) {
		ret = -errno;
		vfio_dev_err(vdev, "failed to open '%s': %s", dir_path, strerror(errno));
		return ret;
	}

	while ((entry = readdir(dir)) != NULL) {
		if (strncmp("vfio", entry->d_name, 4))
			continue;

		if (snprintf(path, path_sz, "%s/%s", VFIO_DEV_DIR,
			     entry->d_name) >= (int)path_sz) {
			ret = -EINVAL;
			break;
		}

		ret = 0;
		break;
	}

	closedir(dir);

	if (ret)
		vfio_dev_err(vdev, "no vfio cdev found under '%s'", dir_path);

	return ret;
}

static void vfio_iommufd_group_put(struct vfio_device *vdev)
{
	struct vfio_iommufd_group *group = vdev->iommufd_group;

	if (!group)
		return;

	if (--group->refs == 0) {
		vfio_iommufd_group_destroy_hwpt(group);
		list_del(&group->list);
		free(group);
	}

	vdev->iommufd_group = NULL;
}

static int vfio_iommufd_group_get_for_dev(struct vfio_device *vdev)
{
	struct vfio_iommufd_group *group;
	char group_path[PATH_MAX];
	char *group_name;
	unsigned long group_id;
	ssize_t ret;
	int dirfd;

	dirfd = open(vdev->sysfs_path, O_DIRECTORY | O_PATH | O_RDONLY);
	if (dirfd < 0) {
		vfio_dev_err(vdev, "failed to open '%s': %s", vdev->sysfs_path,
			     strerror(errno));
		return -errno;
	}

	ret = readlinkat(dirfd, "iommu_group", group_path, sizeof(group_path));
	close(dirfd);
	if (ret < 0) {
		vfio_dev_err(vdev, "no iommu_group");
		return -errno;
	}
	if (ret == sizeof(group_path))
		return -EINVAL;

	group_path[ret] = '\0';
	group_name = basename(group_path);
	errno = 0;
	group_id = strtoul(group_name, NULL, 10);
	if (errno)
		return -errno;

	list_for_each_entry(group, &iommufd_groups, list) {
		if (group->id == group_id) {
			group->refs++;
			vdev->iommufd_group = group;
			return 0;
		}
	}

	group = calloc(1, sizeof(*group));
	if (!group)
		return -ENOMEM;

	group->id = group_id;
	group->refs = 1;
	list_add(&group->list, &iommufd_groups);
	vdev->iommufd_group = group;

	return 0;
}

static int vfio_iommufd_device_open(struct kvm *kvm, struct vfio_device *vdev)
{
	struct vfio_device_bind_iommufd bind = {
		.argsz	= sizeof(bind),
		.iommufd = iommufd_ctx,
	};
	char cdev_path[PATH_MAX];
	int ret;

	ret = vfio_iommufd_get_cdev_path(vdev, cdev_path, sizeof(cdev_path));
	if (ret)
		return ret;

	vdev->fd = open(cdev_path, O_RDWR);
	if (vdev->fd < 0) {
		ret = -errno;
		vfio_dev_err(vdev, "failed to open '%s': %s", cdev_path, strerror(errno));
		return ret;
	}

	if (ioctl(vdev->fd, VFIO_DEVICE_BIND_IOMMUFD, &bind)) {
		ret = -errno;
		vfio_dev_err(vdev, "failed to bind to iommufd: %s", strerror(errno));
		close(vdev->fd);
		vdev->fd = -1;
		return ret;
	}

	vdev->iommufd_id = bind.out_devid;

	return 0;
}

/*
 * IOMMU_HWPT_ALLOC_NEST_PARENT selects the RISC-V IOMMU driver's
 * second-stage (GSCID/iohgatp) domain. An implicit IOAS attachment uses
 * flags=0 and selects a first-stage domain. Allocate one HWPT explicitly
 * and share it among the cdevs in the same IOMMU group. No nested child
 * page table or guest vIOMMU is needed to use this second-stage domain.
 */
static int vfio_iommufd_device_attach(struct kvm *kvm, struct vfio_device *vdev)
{
	struct vfio_iommufd_group *group = vdev->iommufd_group;
	struct iommu_hwpt_alloc hwpt_alloc = {
		.size		= sizeof(hwpt_alloc),
		.flags		= IOMMU_HWPT_ALLOC_NEST_PARENT,
		.dev_id		= vdev->iommufd_id,
		.pt_id		= iommufd_ioas_id,
		.data_type	= IOMMU_HWPT_DATA_NONE,
	};
	struct vfio_device_attach_iommufd_pt attach = {
		.argsz	= sizeof(attach),
	};
	int ret;

	if (!group->hwpt_id) {
		if (ioctl(iommufd_ctx, IOMMU_HWPT_ALLOC, &hwpt_alloc)) {
			ret = -errno;
			vfio_dev_err(vdev, "failed to allocate HWPT: %s", strerror(errno));
			return ret;
		}

		group->hwpt_id = hwpt_alloc.out_hwpt_id;
		group->hwpt_vdev = vdev;
	}

	attach.pt_id = group->hwpt_id;

	if (ioctl(vdev->fd, VFIO_DEVICE_ATTACH_IOMMUFD_PT, &attach)) {
		ret = -errno;
		vfio_dev_err(vdev, "failed to attach to HWPT: %s", strerror(errno));
		return ret;
	}

	vdev->hwpt_id = group->hwpt_id;
	group->hwpt_users++;

	return 0;
}

static void vfio_iommufd_device_detach(struct vfio_device *vdev)
{
	struct vfio_device_detach_iommufd_pt detach = {
		.argsz	= sizeof(detach),
	};

	if (!vdev->hwpt_id)
		return;

	if (ioctl(vdev->fd, VFIO_DEVICE_DETACH_IOMMUFD_PT, &detach))
		vfio_dev_warn(vdev, "failed to detach from HWPT: %s", strerror(errno));
}

static void vfio_iommufd_device_hwpt_put(struct vfio_device *vdev)
{
	struct vfio_iommufd_group *group = vdev->iommufd_group;

	if (!vdev->hwpt_id)
		return;

	vdev->hwpt_id = 0;
	if (--group->hwpt_users == 0)
		vfio_iommufd_group_destroy_hwpt(group);
}

/*
 * Fetch device info/regions and do bus-specific setup. Identical to
 * what the legacy VFIO type1 container path does in
 * vfio_configure_device() once it has a device fd -- this backend
 * only differs in how that fd was obtained (BIND_IOMMUFD + cdev vs.
 * VFIO_GROUP_GET_DEVICE_FD).
 */
static int vfio_iommufd_device_configure(struct kvm *kvm, struct vfio_device *vdev)
{
	int ret;

	vdev->info.argsz = sizeof(vdev->info);
	if (ioctl(vdev->fd, VFIO_DEVICE_GET_INFO, &vdev->info)) {
		ret = -errno;
		vfio_dev_err(vdev, "failed to get info: %s", strerror(errno));
		return ret;
	}

	if (vdev->info.flags & VFIO_DEVICE_FLAGS_RESET &&
	    ioctl(vdev->fd, VFIO_DEVICE_RESET) < 0)
		vfio_dev_warn(vdev, "failed to reset device");

	vdev->regions = calloc(vdev->info.num_regions, sizeof(*vdev->regions));
	if (!vdev->regions)
		return -ENOMEM;

	switch (vdev->params->type) {
	case VFIO_DEVICE_PCI:
		BUG_ON(!(vdev->info.flags & VFIO_DEVICE_FLAGS_PCI));
		ret = vfio_pci_setup_device(kvm, vdev);
		break;
	default:
		BUG_ON(1);
		ret = -EINVAL;
	}

	if (ret) {
		free(vdev->regions);
		vdev->regions = NULL;
		return ret;
	}

	vdev->iommufd_configured = true;

	vfio_dev_info(vdev, "assigned to device number 0x%x",
		     vdev->dev_hdr.dev_num);

	return 0;
}

static void vfio_iommufd_device_exit(struct kvm *kvm, struct vfio_device *vdev)
{
	if (vdev->fd >= 0) {
		if (vdev->iommufd_configured) {
			switch (vdev->params->type) {
			case VFIO_DEVICE_PCI:
				vfio_pci_teardown_device(kvm, vdev);
				break;
			default:
				vfio_dev_warn(vdev, "no teardown function for device");
			}
		}

		vfio_iommufd_device_detach(vdev);
		close(vdev->fd);
		vdev->fd = -1;
	}

	vfio_iommufd_device_hwpt_put(vdev);
	free(vdev->regions);
	vdev->regions = NULL;
	vdev->iommufd_configured = false;
	vfio_iommufd_group_put(vdev);
	free(vdev->sysfs_path);
	vdev->sysfs_path = NULL;
}

static int vfio_iommufd_device_init(struct kvm *kvm, struct vfio_device *vdev)
{
	char dev_path[PATH_MAX];
	int ret;

	vdev->fd = -1;

	ret = snprintf(dev_path, PATH_MAX, "/sys/bus/%s/devices/%s",
		       vdev->params->bus, vdev->params->name);
	if (ret < 0 || ret == PATH_MAX)
		return -EINVAL;

	vdev->sysfs_path = strndup(dev_path, PATH_MAX);
	if (!vdev->sysfs_path)
		return -errno;

	ret = vfio_iommufd_group_get_for_dev(vdev);
	if (ret)
		goto err_free_sysfs_path;

	ret = vfio_iommufd_device_open(kvm, vdev);
	if (ret)
		goto err_exit_device;

	ret = vfio_iommufd_device_attach(kvm, vdev);
	if (ret)
		goto err_exit_device;

	ret = vfio_iommufd_device_configure(kvm, vdev);
	if (ret)
		goto err_exit_device;

	return 0;

err_exit_device:
	vfio_iommufd_device_exit(kvm, vdev);
	return ret;

err_free_sysfs_path:
	free(vdev->sysfs_path);
	vdev->sysfs_path = NULL;
	return ret;
}

static int vfio_iommufd_configure_devices(struct kvm *kvm)
{
	int i, ret;

	for (i = 0; i < kvm->cfg.num_vfio_devices; ++i) {
		iommufd_devices[i].params = &kvm->cfg.vfio_devices[i];
		iommufd_num_devices = i + 1;

		ret = vfio_iommufd_device_init(kvm, &iommufd_devices[i]);
		if (ret)
			return ret;
	}

	return 0;
}

static void vfio_iommufd_devices_exit(struct kvm *kvm)
{
	int i;

	for (i = 0; i < iommufd_num_devices; i++)
		vfio_iommufd_device_exit(kvm, &iommufd_devices[i]);

	free(iommufd_devices);
	iommufd_devices = NULL;
	iommufd_num_devices = 0;

	if (iommufd_ctx >= 0) {
		close(iommufd_ctx);
		iommufd_ctx = -1;
	}
}

/*
 * Attaching a device narrows the IOAS's allowed IOVA ranges by
 * whatever reserved regions that device's IOMMU driver publishes
 * (e.g. RISC-V's fixed SW-MSI window for IMSIC remapping). That
 * narrowing only takes effect once a device is actually attached, so
 * this must run after every device has gone through
 * vfio_iommufd_device_attach() and before any guest RAM is mapped --
 * mapping first and attaching after would let RAM silently land on a
 * range a device then can't be attached against.
 *
 * IOMMU_IOAS_IOVA_RANGES is a two-call protocol: an initial call
 * measures how many ranges exist, then a second call with a
 * sufficiently sized buffer fetches them.
 */
struct vfio_iommufd_iova_ranges {
	u32				nr_ranges;
	struct iommu_iova_range		ranges[];
};

static int vfio_iommufd_check_ram_bank(struct kvm *kvm, struct kvm_mem_bank *bank,
				       void *data)
{
	struct vfio_iommufd_iova_ranges *ranges = data;
	u64 bank_last = bank->guest_phys_addr + bank->size - 1;
	unsigned int i;

	for (i = 0; i < ranges->nr_ranges; i++) {
		if (bank->guest_phys_addr >= ranges->ranges[i].start &&
		    bank_last <= ranges->ranges[i].last)
			return 0;
	}

	pr_err("Guest RAM bank 0x%llx-0x%llx is outside the IOAS's allowed IOVA ranges "
	       "(likely colliding with a device's reserved IOVA window, e.g. the IOMMU's SW-MSI region)",
	       (unsigned long long)bank->guest_phys_addr, (unsigned long long)bank_last);

	return -EINVAL;
}

static int vfio_iommufd_check_ram_layout(struct kvm *kvm)
{
	struct iommu_ioas_iova_ranges ranges_query = {
		.size	= sizeof(ranges_query),
		.ioas_id = iommufd_ioas_id,
	};
	struct vfio_iommufd_iova_ranges *ranges;
	int ret;

	/*
	 * The first call is expected to fail with -EMSGSIZE: passing
	 * num_iovas=0 asks the kernel only to report how many ranges
	 * exist (via the updated num_iovas), not to fetch them.
	 */
	if (ioctl(iommufd_ctx, IOMMU_IOAS_IOVA_RANGES, &ranges_query) &&
	    errno != EMSGSIZE) {
		ret = -errno;
		pr_err("Failed to query IOAS IOVA ranges: %s", strerror(errno));
		return ret;
	}

	ranges = malloc(sizeof(*ranges) +
			ranges_query.num_iovas * sizeof(struct iommu_iova_range));
	if (!ranges)
		return -ENOMEM;

	ranges->nr_ranges = ranges_query.num_iovas;
	ranges_query.num_iovas = ranges->nr_ranges;
	ranges_query.allowed_iovas = (unsigned long)ranges->ranges;

	if (ioctl(iommufd_ctx, IOMMU_IOAS_IOVA_RANGES, &ranges_query)) {
		ret = -errno;
		pr_err("Failed to fetch IOAS IOVA ranges: %s", strerror(errno));
		free(ranges);
		return ret;
	}

	ret = kvm__for_each_mem_bank(kvm, KVM_MEM_TYPE_RAM,
				     vfio_iommufd_check_ram_bank, ranges);
	free(ranges);

	return ret;
}

static int vfio_iommufd_map_mem_bank(struct kvm *kvm, struct kvm_mem_bank *bank,
				     void *data)
{
	struct iommu_ioas_map map = {
		.size	= sizeof(map),
		.flags	= IOMMU_IOAS_MAP_FIXED_IOVA | IOMMU_IOAS_MAP_WRITEABLE |
			  IOMMU_IOAS_MAP_READABLE,
		.ioas_id = iommufd_ioas_id,
		.user_va = (unsigned long)bank->host_addr,
		.iova	= bank->guest_phys_addr,
		.length	= bank->size,
	};
	int ret;

	if (ioctl(iommufd_ctx, IOMMU_IOAS_MAP, &map)) {
		ret = -errno;
		pr_err("Failed to map 0x%llx -> 0x%llx (%llu) for DMA: %s",
		       (unsigned long long)map.iova, (unsigned long long)map.user_va,
		       (unsigned long long)map.length, strerror(errno));
		return ret;
	}

	iommufd_num_mapped_banks++;

	return 0;
}

static int vfio_iommufd_unmap_mem_bank(struct kvm *kvm, struct kvm_mem_bank *bank,
				       void *data)
{
	struct iommu_ioas_unmap unmap = {
		.size	= sizeof(unmap),
		.ioas_id = iommufd_ioas_id,
		.iova	= bank->guest_phys_addr,
		.length	= bank->size,
	};

	if (!iommufd_num_mapped_banks)
		return 0;

	ioctl(iommufd_ctx, IOMMU_IOAS_UNMAP, &unmap);
	iommufd_num_mapped_banks--;

	return 0;
}

int vfio_iommufd__init(struct kvm *kvm)
{
	int ret;

	iommufd_devices = calloc(kvm->cfg.num_vfio_devices,
				 sizeof(*iommufd_devices));
	if (!iommufd_devices)
		return -ENOMEM;

	ret = vfio_iommufd_container_init(kvm);
	if (ret)
		goto err_exit_devices;

	ret = vfio_iommufd_configure_devices(kvm);
	if (ret)
		goto err_exit_devices;

	ret = vfio_iommufd_check_ram_layout(kvm);
	if (ret)
		goto err_exit_devices;

	ret = kvm__for_each_mem_bank(kvm, KVM_MEM_TYPE_RAM,
				      vfio_iommufd_map_mem_bank, NULL);
	if (!ret)
		return 0;

	kvm__for_each_mem_bank(kvm, KVM_MEM_TYPE_RAM,
				vfio_iommufd_unmap_mem_bank, NULL);

err_exit_devices:
	vfio_iommufd_devices_exit(kvm);
	return ret;
}

int vfio_iommufd__exit(struct kvm *kvm)
{
	kvm__for_each_mem_bank(kvm, KVM_MEM_TYPE_RAM,
			       vfio_iommufd_unmap_mem_bank, NULL);

	vfio_iommufd_devices_exit(kvm);

	free(kvm->cfg.vfio_devices);

	return 0;
}
