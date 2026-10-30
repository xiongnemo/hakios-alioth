// IPA Installer (IPA 安装): the phone's own way to put applications on it -- from a URL, from Files, or an IPA
// "opened in" it from Safari or Files. It runs as mobile and cannot write the bundle directories, so what it is
// asked it leaves as a request in the spool of the service com.hakios.apps (src/ipainstall.m -d, root), and shows
// that service's answer as it comes: SPOOL/queue/ID.plist in, SPOOL/results/ID.plist out, SPOOL/installed.plist
// the list. Built by build.sh into /redwood/apps/IPAInstaller.app, which the same service registers at boot.
// Its words are English here and in en.lproj, Chinese in zh-Hans.lproj (Localizable.strings, InfoPlist.strings).
#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <sys/stat.h>

#define SPOOL @"/private/var/mobile/Library/HakiosInstall"
#define L(text) NSLocalizedString(text, nil)

@interface InstallerController : UITableViewController <UIDocumentPickerDelegate>
@property (copy) NSArray<NSDictionary *> *apps;
@property (copy) NSString *status;
@property (copy) NSString *pending;            // the request being served
@property (copy) NSString *pending_file;       // a copy of ours to remove when it is done
@property (copy) NSString *pending_what;
@property (strong) NSDate *pending_since;
@property (strong) NSTimer *timer;
- (void)installFile:(NSURL *)url;
@end

@implementation InstallerController

- (void)viewDidLoad
{
	[super viewDidLoad];
	self.title = L(@"IPA Installer");
	self.status = L(@"Apps installed here disappear when the phone restarts (this iOS keeps its data in memory).");
	[self reloadApps];
	self.timer = [NSTimer scheduledTimerWithTimeInterval:0.5 target:self selector:@selector(poll) userInfo:nil
	    repeats:YES];
}

- (void)reloadApps
{
	NSArray *list = [NSArray arrayWithContentsOfFile:[SPOOL stringByAppendingPathComponent:@"installed.plist"]];
	NSMutableArray *apps = [NSMutableArray array];

	for (NSDictionary *app in list) {
		if (![app[@"BundleID"] isEqualToString:NSBundle.mainBundle.bundleIdentifier]) {
			[apps addObject:app];
		}
	}
	self.apps = apps;
}

// ---------------------------------------------------------------- requests to the service

- (void)submit:(NSDictionary *)request what:(NSString *)what file:(NSString *)file
{
	if (self.pending != nil) {
		[self tell:L(@"Please wait") message:L(@"The previous one is still being installed.")];
		return;
	}
	NSString *rid = NSUUID.UUID.UUIDString;
	NSString *queue = [SPOOL stringByAppendingPathComponent:@"queue"];
	NSString *tmp = [queue stringByAppendingPathComponent:[@"." stringByAppendingString:rid]];
	NSString *path = [queue stringByAppendingPathComponent:[rid stringByAppendingPathExtension:@"plist"]];

	// (written aside and renamed in, so the service never reads half of it)
	if (![request writeToFile:tmp atomically:NO] || rename(tmp.fileSystemRepresentation, path.fileSystemRepresentation) != 0) {
		[self tell:L(@"Installer service unavailable")
		    message:[NSString stringWithFormat:L(@"Cannot write %@ (%@)"), queue, @(strerror(errno))]];
		return;
	}
	self.pending = rid;
	self.pending_file = file;
	self.pending_what = what;
	self.pending_since = NSDate.date;
	self.status = [NSString stringWithFormat:L(@"%@: waiting for the installer service…"), what];
	[self.tableView reloadData];
}

- (void)poll
{
	if (self.pending == nil) {
		return;
	}
	NSString *path = [[SPOOL stringByAppendingPathComponent:@"results"]
	    stringByAppendingPathComponent:[self.pending stringByAppendingPathExtension:@"plist"]];
	NSDictionary *result = [NSDictionary dictionaryWithContentsOfFile:path];

	if (result == nil) {
		if (-self.pending_since.timeIntervalSinceNow > 15) {
			self.status = [NSString stringWithFormat:
			    L(@"%@: the installer service does not answer (it starts after boot; look again in a moment)"),
			    self.pending_what];
			[self.tableView reloadData];
		}
		return;
	}
	NSString *log = [result[@"Log"] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];

	self.status = [NSString stringWithFormat:L(@"%@:\n%@"), self.pending_what, log.length ? log : @"…"];
	if ([result[@"Done"] boolValue]) {
		BOOL ok = [result[@"OK"] boolValue];

		if (self.pending_file != nil) {
			[NSFileManager.defaultManager removeItemAtPath:self.pending_file error:NULL];
		}
		self.pending = nil;
		self.pending_file = nil;
		[self reloadApps];
		[self tell:ok ? L(@"Done") : L(@"Failed") message:ok ? self.pending_what : log];
	}
	[self.tableView reloadData];
}

- (void)tell:(NSString *)title message:(NSString *)message
{
	UIAlertController *alert = [UIAlertController alertControllerWithTitle:title message:message
	    preferredStyle:UIAlertControllerStyleAlert];

	[alert addAction:[UIAlertAction actionWithTitle:L(@"OK") style:UIAlertActionStyleDefault handler:nil]];
	[self presentViewController:alert animated:YES completion:nil];
}

// ---------------------------------------------------------------- the three ways in

- (void)askForURL
{
	UIAlertController *alert = [UIAlertController alertControllerWithTitle:L(@"Install from URL")
	    message:L(@"Where to download the IPA from (https://…, or http://… on your computer)")
	    preferredStyle:UIAlertControllerStyleAlert];

	[alert addTextFieldWithConfigurationHandler:^(UITextField *field) {
		field.placeholder = @"https://example.com/app.ipa";
		field.keyboardType = UIKeyboardTypeURL;
		field.autocapitalizationType = UITextAutocapitalizationTypeNone;
		field.autocorrectionType = UITextAutocorrectionTypeNo;
		field.clearButtonMode = UITextFieldViewModeWhileEditing;
	}];
	[alert addAction:[UIAlertAction actionWithTitle:L(@"Cancel") style:UIAlertActionStyleCancel handler:nil]];
	[alert addAction:[UIAlertAction actionWithTitle:L(@"Install") style:UIAlertActionStyleDefault
	    handler:^(UIAlertAction *action) {
		NSString *url = [alert.textFields.firstObject.text
		    stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];

		if (![url hasPrefix:@"https://"] && ![url hasPrefix:@"http://"]) {
			[self tell:L(@"Not a URL") message:L(@"It has to start with https:// or http://.")];
			return;
		}
		[self submit:@{ @"Action": @"install", @"Source": url } what:url.lastPathComponent.stringByRemovingPercentEncoding
		    file:nil];
	}]];
	[self presentViewController:alert animated:YES completion:nil];
}

- (void)pickFile
{
	UTType *ipa = [UTType typeWithIdentifier:@"com.apple.itunes.ipa"] ?: [UTType typeWithFilenameExtension:@"ipa"];
	UIDocumentPickerViewController *picker = [[UIDocumentPickerViewController alloc]
	    initForOpeningContentTypes:@[ ipa ?: UTTypeData ] asCopy:YES];

	picker.delegate = self;
	[self presentViewController:picker animated:YES completion:nil];
}

- (void)documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls
{
	if (urls.firstObject != nil) {
		[self installFile:urls.firstObject];
	}
}

// a file of ours (picked as a copy, or "opened in" this application: Documents/Inbox)
- (void)installFile:(NSURL *)url
{
	NSString *path = url.path;

	// (the service runs as root and reads it where it is; it must be a plain file that stays until it is done)
	if (url.isFileURL && path != nil) {
		chmod(path.fileSystemRepresentation, 0644);
		[self submit:@{ @"Action": @"install", @"Source": path } what:path.lastPathComponent file:path];
	}
}

- (void)uninstall:(NSDictionary *)app
{
	UIAlertController *alert = [UIAlertController
	    alertControllerWithTitle:[NSString stringWithFormat:L(@"Uninstall “%@”?"), app[@"Name"]]
	    message:L(@"Its data is deleted with it.") preferredStyle:UIAlertControllerStyleAlert];

	[alert addAction:[UIAlertAction actionWithTitle:L(@"Cancel") style:UIAlertActionStyleCancel handler:nil]];
	[alert addAction:[UIAlertAction actionWithTitle:L(@"Uninstall") style:UIAlertActionStyleDestructive
	    handler:^(UIAlertAction *action) {
		[self submit:@{ @"Action": @"uninstall", @"BundleID": app[@"BundleID"] }
		    what:[NSString stringWithFormat:L(@"Uninstall %@"), app[@"Name"]] file:nil];
	}]];
	[self presentViewController:alert animated:YES completion:nil];
}

// ---------------------------------------------------------------- the table: what to do, what is there, how it went

- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView
{
	return 3;
}

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section
{
	return section == 0 ? 2 : section == 1 ? MAX(1, (NSInteger)self.apps.count) : 1;
}

- (NSString *)tableView:(UITableView *)tableView titleForHeaderInSection:(NSInteger)section
{
	if (section == 0) {
		return L(@"Install");
	}
	return section == 1 ? [NSString stringWithFormat:L(@"Installed (%lu)"), (unsigned long)self.apps.count] : L(@"Status");
}

- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section
{
	if (section == 0) {
		return L(@"You can also open an .ipa in Safari or Files, tap Share and choose IPA Installer. "
		    "No signature is needed; the IPA must not be encrypted.");
	}
	return section == 1 && self.apps.count ? L(@"Swipe left to uninstall.") : nil;
}

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath
{
	UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:nil];
	UIListContentConfiguration *content = cell.defaultContentConfiguration;

	if (indexPath.section == 0) {
		content.text = indexPath.row == 0 ? L(@"Install from URL…") : L(@"Choose an IPA in Files…");
		content.image = [UIImage systemImageNamed:indexPath.row == 0 ? @"link" : @"folder"];
		content.textProperties.color = self.view.tintColor;
	} else if (indexPath.section == 1) {
		if (self.apps.count == 0) {
			content.text = L(@"None yet");
			content.textProperties.color = UIColor.secondaryLabelColor;
		} else {
			NSDictionary *app = self.apps[indexPath.row];

			content.text = app[@"Name"];
			content.secondaryText = [NSString stringWithFormat:@"%@  %@", app[@"BundleID"], app[@"Version"]];
			content.image = [UIImage systemImageNamed:@"app"];
		}
		cell.selectionStyle = UITableViewCellSelectionStyleNone;
	} else {
		content.text = self.status;
		content.textProperties.numberOfLines = 0;
		content.textProperties.font = [UIFont monospacedSystemFontOfSize:12 weight:UIFontWeightRegular];
		cell.selectionStyle = UITableViewCellSelectionStyleNone;
	}
	cell.contentConfiguration = content;
	return cell;
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath
{
	[tableView deselectRowAtIndexPath:indexPath animated:YES];
	if (indexPath.section == 0) {
		indexPath.row == 0 ? [self askForURL] : [self pickFile];
	}
}

- (UISwipeActionsConfiguration *)tableView:(UITableView *)tableView
    trailingSwipeActionsConfigurationForRowAtIndexPath:(NSIndexPath *)indexPath
{
	if (indexPath.section != 1 || self.apps.count == 0) {
		return nil;
	}
	NSDictionary *app = self.apps[indexPath.row];
	UIContextualAction *remove = [UIContextualAction contextualActionWithStyle:UIContextualActionStyleDestructive
	    title:L(@"Uninstall") handler:^(UIContextualAction *action, UIView *view, void (^done)(BOOL)) {
		[self uninstall:app];
		done(YES);
	}];

	return [UISwipeActionsConfiguration configurationWithActions:@[ remove ]];
}

@end

@interface InstallerDelegate : UIResponder <UIApplicationDelegate>
@property (strong, nonatomic) UIWindow *window;
@property (strong) InstallerController *controller;
@end

@implementation InstallerDelegate

- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)options
{
	UINavigationController *navigation;

	self.controller = [[InstallerController alloc] initWithStyle:UITableViewStyleInsetGrouped];
	navigation = [[UINavigationController alloc] initWithRootViewController:self.controller];
	navigation.navigationBar.prefersLargeTitles = YES;
	self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
	self.window.rootViewController = navigation;
	[self.window makeKeyAndVisible];
	return YES;
}

// an IPA "opened in" this application from Safari or Files: a copy in Documents/Inbox
- (BOOL)application:(UIApplication *)application openURL:(NSURL *)url options:(NSDictionary *)options
{
	[self.controller installFile:url];
	return YES;
}

@end

int
main(int argc, char *argv[])
{
	@autoreleasepool {
		return UIApplicationMain(argc, argv, nil, NSStringFromClass(InstallerDelegate.class));
	}
}
