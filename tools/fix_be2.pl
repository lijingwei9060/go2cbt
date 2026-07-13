use strict;
use warnings;
use utf8;
use open ':std', ':encoding(UTF-8)';

my $file = 'client/BackupEngine.cpp';
open(my $fh, '<:encoding(UTF-8)', $file) or die "open: $!";
my @lines = <$fh>;
close($fh);

# Strip BOM from first line if present
$lines[0] =~ s/^\x{FEFF}//;

my @out;

# State machine for editing
my $in_fullbackup_vss = 0;
my $in_fullbackup_loop = 0;
my $in_fullbackup_hasfs = 0;
my $in_incr_vss = 0;
my $in_incr_loop = 0;

for (my $i = 0; $i < @lines; $i++) {
    my $line = $lines[$i];

    # === FULLBACKUP ===

    # After vss.Initialize() success block (the closing }), insert StartSnapshotSet
    if ($line =~ /^\t\t\treturn false;/ && $lines[$i+1] =~ /^\t\t\}$/ &&
        $lines[$i-1] =~ /LOG_ERROR.*VSS initialization failed/) {
        push @out, $line;
        $i++; push @out, $lines[$i]; # the closing }
        # Insert StartSnapshotSet
        push @out, "\n";
        push @out, "\t\t// 启动 VSS 快照集（必须在 AddVolumeToSnapshotSet 之前调用）\n";
        push @out, "\t\tif (!vss.StartSnapshotSet())\n";
        push @out, "\t\t{\n";
        push @out, "\t\t\tLOG_ERROR(L\"[BackupEngine] VSS StartSnapshotSet failed\");\n";
        push @out, "\t\t\tvss.Cleanup();\n";
        push @out, "\t\t\treturn false;\n";
        push @out, "\t\t}\n";
        next;
    }

    # Remove "Calling SetBackupState..." debug block in FullBackup
    if ($line =~ /L"\[BackupEngine\] Calling SetBackupState/) {
        # Skip this line and the following lines until the closing of this debug block + SetBackupState call
        while ($i < @lines) {
            $i++;
            last if $lines[$i] =~ /L"\[BackupEngine\] SetBackupState done/;
        }
        # Skip until after the SetBackupState call closing }
        while ($i < @lines) {
            $i++;
            last if $lines[$i] =~ /^\t\t\tif \(!vss\.DoSnapshotSet/;
        }
        push @out, $lines[$i]; # the DoSnapshotSet line
        next;
    }

    # Skip "vss.SetBackupState();" lines
    if ($line =~ /^\t\t\tvss\.SetBackupState\(\);$/ || $line =~ /^\t\tvss\.SetBackupState\(\);$/) {
        next;
    }

    # Add VSS abort check before "if (hasFilesystem)" in FullBackup
    # Look for the snapshot summary closing } followed by if(hasFilesystem)
    if ($line =~ /^\t\tif \(hasFilesystem\)$/ && $in_fullbackup_vss) {
        push @out, "\t\t// VSS 快照失败则中止备份\n";
        push @out, "\t\tif (fsSkipped > 0 && fsAdded == 0)\n";
        push @out, "\t\t{\n";
        push @out, "\t\t\tLOG_ERROR(L\"[BackupEngine] VSS snapshot set failed - all volumes rejected, aborting backup\");\n";
        push @out, "\t\t\tvss.Cleanup();\n";
        push @out, "\t\t\treturn false;\n";
        push @out, "\t\t}\n";
        push @out, "\n";
        push @out, $line;
        next;
    }

    # === INCREMENTAL BACKUP ===

    # Track when we're in the IncrementalBackup VSS section
    if ($line =~ /\/\/ ---- VSS 快照 ----/) {
        $in_fullbackup_vss = 0;
        $in_incr_vss = 1;
    }

    # After IncrementalBackup Initialize() success, insert StartSnapshotSet
    if ($in_incr_vss && $line =~ /^\t\t\tcbt\.Disconnect\(\);$/ && $lines[$i+1] =~ /^\t\t\treturn false;$/ && $lines[$i+2] =~ /^\t\t\}$/) {
        push @out, $line;  # cbt.Disconnect();
        $i++; push @out, $lines[$i]; # return false;
        $i++; push @out, $lines[$i]; # }
        # Insert StartSnapshotSet
        push @out, "\n";
        push @out, "\t\t// 启动 VSS 快照集（必须在 AddVolumeToSnapshotSet 之前调用）\n";
        push @out, "\t\tif (!vss.StartSnapshotSet())\n";
        push @out, "\t\t{\n";
        push @out, "\t\t\tLOG_ERROR(L\"[BackupEngine] VSS StartSnapshotSet failed for incremental\");\n";
        push @out, "\t\t\tvss.Cleanup();\n";
        push @out, "\t\t\tcbt.Disconnect();\n";
        push @out, "\t\t\treturn false;\n";
        push @out, "\t\t}\n";
        next;
    }

    # Fix IncrementalBackup volume loop:
    # "bool hasFilesystem = false;" followed by for loop
    if ($in_incr_vss && $line =~ /^\t\tbool hasFilesystem = false;$/) {
        push @out, $line;
        push @out, "\t\tint fsAdded = 0, fsSkipped = 0;\n";
        next;
    }

    # Inside IncrementalBackup volume loop: fix AddVolumeToSnapshotSet call
    if ($in_incr_vss && $line =~ /^\t\t\t\tvss\.AddVolumeToSnapshotSet\(mp\.VolumeGuid, setId\);$/) {
        push @out, "\t\t\t\tif (vss.AddVolumeToSnapshotSet(mp.VolumeGuid, setId))\n";
        push @out, "\t\t\t\t{\n";
        push @out, "\t\t\t\t\thasFilesystem = true;\n";
        push @out, "\t\t\t\t\tfsAdded++;\n";
        push @out, "\t\t\t\t}\n";
        push @out, "\t\t\t\telse\n";
        push @out, "\t\t\t\t{\n";
        push @out, "\t\t\t\t\tfsSkipped++;\n";
        push @out, "\t\t\t\t}\n";
        next;
    }

    # Skip old "hasFilesystem = true;" that follows the AddVolumeToSnapshotSet call
    if ($in_incr_vss && $line =~ /^\t\t\t\thasFilesystem = true;$/) {
        next;
    }

    # Fix IncrementalBackup if(hasFilesystem) block: add abort check and DoSnapshotSet check
    if ($in_incr_vss && $line =~ /^\t\tif \(hasFilesystem\)$/ && $lines[$i+1] =~ /^\t\t\{$/ && $lines[$i+2] =~ /^\t\t\tvss\.DoSnapshotSet\(\);$/) {
        push @out, "\t\t// VSS 快照失败则中止备份\n";
        push @out, "\t\tif (fsSkipped > 0 && fsAdded == 0)\n";
        push @out, "\t\t{\n";
        push @out, "\t\t\tLOG_ERROR(L\"[BackupEngine] VSS snapshot set failed - all volumes rejected, aborting incremental backup\");\n";
        push @out, "\t\t\tvss.Cleanup();\n";
        push @out, "\t\t\tcbt.Disconnect();\n";
        push @out, "\t\t\treturn false;\n";
        push @out, "\t\t}\n";
        push @out, "\n";
        push @out, $line;  # if (hasFilesystem)
        $i++; push @out, $lines[$i]; # {
        $i++; # skip old DoSnapshotSet
        push @out, "\t\t\tif (!vss.DoSnapshotSet())\n";
        push @out, "\t\t\t{\n";
        push @out, "\t\t\t\tLOG_ERROR(L\"[BackupEngine] VSS DoSnapshotSet failed for incremental\");\n";
        push @out, "\t\t\t\tvss.Cleanup();\n";
        push @out, "\t\t\t\tcbt.Disconnect();\n";
        push @out, "\t\t\t\treturn false;\n";
        push @out, "\t\t\t}\n";
        next;
    }

    # Track FullBackup VSS section
    if ($line =~ /\/\/ ---- VSS 快照 ----/) {
        $in_fullbackup_vss = 1;
    }

    push @out, $line;
}

# Write back
open(my $out, '>:encoding(UTF-8)', $file) or die "write: $!";
print $out "\x{FEFF}";
print $out @out;
close($out);
print "OK: BackupEngine.cpp written\n";
