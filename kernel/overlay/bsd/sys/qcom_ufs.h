/*
 * Qualcomm UFSHCI (UFS 2.2 Host Controller) block device: the system image's partition read
 * on demand through UTRD/UPIU SCSI READ (10) commands when ufs_root=1 is passed.
 * See bsd/dev/qcom_ufs.c.
 */
#ifndef _SYS_QCOM_UFS_H_
#define _SYS_QCOM_UFS_H_

#include <sys/types.h>

__BEGIN_DECLS
void qcom_ufs_init(void);              /* probe controller if ufs_root=1 is in boot-args */
int qcom_ufs_root(dev_t *root_dev);    /* ufs_root=1: root block device; 0 or an errno */
void qcom_ufs_devfs(void);             /* /dev/ufs0, /dev/rufs0 once devfs is up */
__END_DECLS

#endif /* _SYS_QCOM_UFS_H_ */
