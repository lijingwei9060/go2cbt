use strict;
use warnings;
use utf8;
use open ':std', ':encoding(UTF-8)';

my $file = 'client/BackupEngine.cpp';
open(my $fh, '<:encoding(UTF-8)', $file) or die "open: $!";
my $c = do { local $/; <$fh> };
close($fh);

my $changed = 0;

# === FullBackup: Insert StartSnapshotSet after vss.Initialize() success ===
my $m1 = "\t\tif (!vss.Initialize())\n\n\t\t{\n\n\t\t\tLOG_ERROR(L\"[BackupEngine] VSS initialization failed\");\n\n\t\t\treturn false;\n\n\t\t}\n\n\n\n\t\t// 卷映射";
my $r1 = <<'END';
\t\tif (!vss.Initialize())

\t\t{

\t\t\tLOG_ERROR(L"[BackupEngine] VSS initialization failed");

\t\t\treturn false;

\t\t}

\t\t// 启动 VSS 快照集（必须在 AddVolumeToSnapshotSet 之前调用）
\t\tif (!vss.StartSnapshotSet())
\t\t{
\t\t\tLOG_ERROR(L"[BackupEngine] VSS StartSnapshotSet failed");
\t\t\tvss.Cleanup();
\t\t\treturn false;
\t\t}

\t\t// 卷映射
END
$r1 =~ s/\\t/\t/g;

if ($c =~ s/\Q$m1\E/$r1/) {
    $changed++;
    print "OK 1: StartSnapshotSet inserted in FullBackup\n";
} else {
    die "FAIL 1: StartSnapshotSet marker not found";
}

# === FullBackup: Remove SetBackupState debug block from if(hasFilesystem) ===
my $m2 = <<'END';
\t\tif (hasFilesystem)
\t\t{
\t\t\t{
\t\t\t\twchar_t dbg_vss[128];
\t\t\t\tswprintf_s(dbg_vss, L"[BackupEngine] Calling SetBackupState...");
\t\t\t\tLOG_DEBUG(dbg_vss);
\t\t\t}
\t\t\tvss.SetBackupState();
\t\t\t{
\t\t\t\twchar_t dbg_vss2[256];
\t\t\t\tswprintf_s(dbg_vss2, L"[BackupEngine] SetBackupState done, calling DoSnapshotSet (COW snapshot, 9 VSS writers)...");
\t\t\t\tLOG_DEBUG(dbg_vss2);
\t\t\t}
\t\t\tif (!vss.DoSnapshotSet())
END
$m2 =~ s/\\t/\t/g;

my $r2 = <<'END';
\t\tif (hasFilesystem)
\t\t{
\t\t\tif (!vss.DoSnapshotSet())
END
$r2 =~ s/\\t/\t/g;

if ($c =~ s/\Q$m2\E/$r2/) {
    $changed++;
    print "OK 2: SetBackupState block removed from FullBackup\n";
} else {
    warn "WARN 2: SetBackupState block pattern not matched (may be OK if already removed)";
}

# === FullBackup: Add VSS abort check after snapshot summary ===
my $m3 = <<'END';
\t\t\tLOG_DEBUG(dbg);

\t\t}

\t\tif (hasFilesystem)
END
$m3 =~ s/\\t/\t/g;

my $r3 = <<'END';
\t\t\tLOG_DEBUG(dbg);

\t\t}

\t\t// VSS 快照失败则中止备份
\t\tif (fsSkipped > 0 && fsAdded == 0)
\t\t{
\t\t\tLOG_ERROR(L"[BackupEngine] VSS snapshot set failed - all volumes rejected, aborting backup");
\t\t\tvss.Cleanup();
\t\t\treturn false;
\t\t}

\t\tif (hasFilesystem)
END
$r3 =~ s/\\t/\t/g;

if ($c =~ s/\Q$m3\E/$r3/) {
    $changed++;
    print "OK 3: VSS abort check added to FullBackup\n";
} else {
    warn "WARN 3: VSS abort marker not matched";
}

# === IncrementalBackup: Insert StartSnapshotSet after vss.Initialize() ===
my $m4 = <<'END';
\t\tif (!vss.Initialize())

\t\t{

\t\t\tcbt.Disconnect();

\t\t\treturn false;

\t\t}



\t\tVolumeMapping::VolumeMapper mapper;

\t\tmapper.Map(layout);
END
$m4 =~ s/\\t/\t/g;

my $r4 = <<'END';
\t\tif (!vss.Initialize())

\t\t{

\t\t\tcbt.Disconnect();

\t\t\treturn false;

\t\t}

\t\t// 启动 VSS 快照集（必须在 AddVolumeToSnapshotSet 之前调用）
\t\tif (!vss.StartSnapshotSet())
\t\t{
\t\t\tLOG_ERROR(L"[BackupEngine] VSS StartSnapshotSet failed for incremental");
\t\t\tvss.Cleanup();
\t\t\tcbt.Disconnect();
\t\t\treturn false;
\t\t}

\t\tVolumeMapping::VolumeMapper mapper;

\t\tmapper.Map(layout);
END
$r4 =~ s/\\t/\t/g;

if ($c =~ s/\Q$m4\E/$r4/) {
    $changed++;
    print "OK 4: StartSnapshotSet inserted in IncrementalBackup\n";
} else {
    warn "WARN 4: IncrementalBackup marker not matched";
}

# === IncrementalBackup: Fix volume loop to track fsAdded/fsSkipped ===
my $m5 = <<'END';
\t\tbool hasFilesystem = false;

\t\tfor (const auto\& mp : mapper.GetMappedPartitions())

\t\t{

\t\t\tauto content = mp.Partition.Content;

\t\t\tif (content == Disk::PartitionContent::FilesystemNTFS ||

\t\t\t\tcontent == Disk::PartitionContent::FilesystemFAT32 ||

\t\t\t\tcontent == Disk::PartitionContent::FilesystemExFAT ||

\t\t\t\tcontent == Disk::PartitionContent::FilesystemReFS)

\t\t\t{

\t\t\t\tVSS_ID setId;

\t\t\t\tvss.AddVolumeToSnapshotSet(mp.VolumeGuid, setId);

\t\t\t\thasFilesystem = true;

\t\t\t}

\t\t}

\t\tif (hasFilesystem)

\t\t{

\t\t\tvss.SetBackupState();

\t\t\tvss.DoSnapshotSet();

\t\t}
END
$m5 =~ s/\\t/\t/g;

my $r5 = <<'END';
\t\tbool hasFilesystem = false;
\t\tint fsAdded = 0, fsSkipped = 0;

\t\tfor (const auto\& mp : mapper.GetMappedPartitions())

\t\t{

\t\t\tauto content = mp.Partition.Content;

\t\t\tif (content == Disk::PartitionContent::FilesystemNTFS ||

\t\t\t\tcontent == Disk::PartitionContent::FilesystemFAT32 ||

\t\t\t\tcontent == Disk::PartitionContent::FilesystemExFAT ||

\t\t\t\tcontent == Disk::PartitionContent::FilesystemReFS)

\t\t\t{

\t\t\t\tVSS_ID setId;

\t\t\t\tif (vss.AddVolumeToSnapshotSet(mp.VolumeGuid, setId))
\t\t\t\t{
\t\t\t\t\thasFilesystem = true;
\t\t\t\t\tfsAdded++;
\t\t\t\t}
\t\t\t\telse
\t\t\t\t{
\t\t\t\t\tfsSkipped++;
\t\t\t\t}

\t\t\t}

\t\t}

\t\t// VSS 快照失败则中止备份
\t\tif (fsSkipped > 0 && fsAdded == 0)
\t\t{
\t\t\tLOG_ERROR(L"[BackupEngine] VSS snapshot set failed - all volumes rejected, aborting incremental backup");
\t\t\tvss.Cleanup();
\t\t\tcbt.Disconnect();
\t\t\treturn false;
\t\t}

\t\tif (hasFilesystem)

\t\t{

\t\t\tif (!vss.DoSnapshotSet())
\t\t\t{
\t\t\t\tLOG_ERROR(L"[BackupEngine] VSS DoSnapshotSet failed for incremental");
\t\t\t\tvss.Cleanup();
\t\t\t\tcbt.Disconnect();
\t\t\t\treturn false;
\t\t\t}

\t\t}
END
$r5 =~ s/\\t/\t/g;

if ($c =~ s/\Q$m5\E/$r5/) {
    $changed++;
    print "OK 5: IncrementalBackup volume loop fixed\n";
} else {
    warn "WARN 5: IncrementalBackup volume loop marker not matched";
    # Try to find what's there
    if ($c =~ /bool hasFilesystem = false;.*?VSS_ID setId;/s) {
        print "  (found hasFilesystem + setId in file, pattern mismatch likely whitespace)\n";
    }
}

print "\nTotal changes: $changed\n";

# Write back
open(my $out, '>:encoding(UTF-8)', $file) or die "write: $!";
print $out "\x{FEFF}";
print $out $c;
close($out);
print "Written: $file\n";
