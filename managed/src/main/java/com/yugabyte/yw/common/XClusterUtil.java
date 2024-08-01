// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.common;

import static play.mvc.Http.Status.BAD_REQUEST;

import com.yugabyte.yw.models.Universe;

public class XClusterUtil {
  public static final String MINIMUN_VERSION_DB_XCLUSTER_SUPPORT_STABLE = "2024.1.1.0-b49";
  public static final String MINIMUN_VERSION_DB_XCLUSTER_SUPPORT_PREVIEW = "2.23.0.0-b394";

  public static boolean supportsDbScopedXCluster(Universe universe) {
    // The minimum YBDB version that supports db scoped replication is 2024.1.1.0-b49 stable and
    //   2.23.0.0-b394 for preview.
    return Util.compareYBVersions(
            universe.getUniverseDetails().getPrimaryCluster().userIntent.ybSoftwareVersion,
            MINIMUN_VERSION_DB_XCLUSTER_SUPPORT_STABLE,
            MINIMUN_VERSION_DB_XCLUSTER_SUPPORT_PREVIEW,
            true /* suppressFormatError */)
        >= 0;
  }

  public static void checkDbScopedXClusterSupported(
      Universe sourceUniverse, Universe targetUniverse) {
    // Check YBDB software version.
    if (!supportsDbScopedXCluster(sourceUniverse)) {
      throw new PlatformServiceException(
          BAD_REQUEST,
          String.format(
              "Db scoped XCluster is not supported in this version of the source universe (%s);"
                  + " please upgrade to a stable version >= %s or preview version >= %s",
              sourceUniverse.getUniverseDetails().getPrimaryCluster().userIntent.ybSoftwareVersion,
              MINIMUN_VERSION_DB_XCLUSTER_SUPPORT_STABLE,
              MINIMUN_VERSION_DB_XCLUSTER_SUPPORT_PREVIEW));
    }
    if (!supportsDbScopedXCluster(targetUniverse)) {
      throw new PlatformServiceException(
          BAD_REQUEST,
          String.format(
              "Db scoped XCluster is not supported in this version of the target universe (%s);"
                  + " please upgrade to a stable version >= %s or preview version >= %s",
              targetUniverse.getUniverseDetails().getPrimaryCluster().userIntent.ybSoftwareVersion,
              MINIMUN_VERSION_DB_XCLUSTER_SUPPORT_STABLE,
              MINIMUN_VERSION_DB_XCLUSTER_SUPPORT_PREVIEW));
    }
  }

  public static void dbScopedXClusterPreChecks(Universe sourceUniverse, Universe targetUniverse) {
    checkDbScopedXClusterSupported(sourceUniverse, targetUniverse);

    // TODO: Validate dbIds passed in exist on source universe.
    // TODO: Validate namespace names exist on both source and target universe.
  }
}
