package es.chiteroman.playintegrityfix;

import android.content.pm.PackageInfo;
import android.content.pm.Signature;
import android.os.Build;
import android.os.Parcel;
import android.os.Parcelable;

public final class CustomPackageInfoCreator implements Parcelable.Creator<PackageInfo> {
    private final Parcelable.Creator<PackageInfo> originalCreator;
    private final Signature spoofedSignature;

    public CustomPackageInfoCreator(Parcelable.Creator<PackageInfo> originalCreator, Signature spoofedSignature) {
        this.originalCreator = originalCreator;
        this.spoofedSignature = spoofedSignature;
    }

    @Override
    @SuppressWarnings("deprecation")
    public PackageInfo createFromParcel(Parcel source) {
        PackageInfo packageInfo = originalCreator.createFromParcel(source);
        if (packageInfo != null && "android".equals(packageInfo.packageName)) {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
                if (packageInfo.signingInfo != null) {
                    Signature[] signaturesArray = packageInfo.signingInfo.getApkContentsSigners();
                    if (signaturesArray != null && signaturesArray.length > 0) {
                        signaturesArray[0] = spoofedSignature;
                    }
                }
            } else {
                final Signature[] signatures = packageInfo.signatures;
                if (signatures != null && signatures.length > 0) {
                    signatures[0] = spoofedSignature;
                }
            }
        }
        return packageInfo;
    }

    @Override
    public PackageInfo[] newArray(int size) {
        return originalCreator.newArray(size);
    }
}
