// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/search/instant_service.h"

#include <stddef.h>
#include <string>

#include "base/bind.h"
#include "base/callback.h"
#include "base/files/file_util.h"
#include "base/memory/ptr_util.h"
#include "base/path_service.h"
#include "base/scoped_observation.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/post_task.h"
#include "base/task/thread_pool.h"
#include "base/time/clock.h"
#include "build/build_config.h"
#include "chrome/browser/chrome_notification_types.h"
#include "chrome/browser/ntp_tiles/chrome_most_visited_sites_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/search/background/ntp_custom_background_service_factory.h"
#include "chrome/browser/search/chrome_colors/chrome_colors_service.h"
#include "chrome/browser/search/instant_service_factory.h"
#include "chrome/browser/search/instant_service_observer.h"
#include "chrome/browser/search/most_visited_iframe_source.h"
#include "chrome/browser/search/search.h"
#include "chrome/browser/themes/theme_properties.h"
#include "chrome/browser/themes/theme_service.h"
#include "chrome/browser/themes/theme_service_factory.h"
#include "chrome/browser/ui/webui/favicon_source.h"
#include "chrome/browser/ui/webui/theme_source.h"
#include "chrome/common/chrome_paths.h"
#include "chrome/common/pref_names.h"
#include "chrome/common/search/search.mojom.h"
#include "chrome/common/url_constants.h"
#include "chrome/grit/theme_resources.h"
#include "components/favicon_base/favicon_url_parser.h"
#include "components/ntp_tiles/constants.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/pref_service.h"
#include "components/search/ntp_features.h"
#include "components/search/search_provider_observer.h"
#include "components/sync_preferences/pref_service_syncable.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/notification_service.h"
#include "content/public/browser/notification_types.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/url_data_source.h"
#include "extensions/browser/extension_registry.h"
#include "extensions/common/extension.h"
#include "mojo/public/cpp/bindings/associated_remote.h"
#include "ui/gfx/color_palette.h"
#include "ui/gfx/color_utils.h"

#include "chrome/browser/search/new_tab_page_source.h"

InstantService::InstantService(Profile* profile)
    : profile_(profile),
      most_visited_info_(std::make_unique<InstantMostVisitedInfo>()),
      pref_service_(profile_->GetPrefs()),
      native_theme_(ui::NativeTheme::GetInstanceForNativeUi()),
      background_updated_timestamp_(base::TimeTicks::Now()) {
  // The initialization below depends on a typical set of browser threads. Skip
  // it if we are running in a unit test without the full suite.
  if (!content::BrowserThread::CurrentlyOn(content::BrowserThread::UI))
    return;

  registrar_.Add(this,
                 content::NOTIFICATION_RENDERER_PROCESS_CREATED,
                 content::NotificationService::AllSources());
  registrar_.Add(this,
                 content::NOTIFICATION_RENDERER_PROCESS_TERMINATED,
                 content::NotificationService::AllSources());

  most_visited_sites_ = ChromeMostVisitedSitesFactory::NewForProfile(profile_);
  LOG(INFO) << "[Kiwi] InstantService::InstantService: " << most_visited_sites_;
  if (most_visited_sites_) {
    most_visited_sites_->AddMostVisitedURLsObserver(
        this, ntp_tiles::kMaxNumMostVisited);
  }

  custom_background_service_ =
      NtpCustomBackgroundServiceFactory::GetForProfile(profile_);

  // Listen for theme installation.
  ThemeServiceFactory::GetForProfile(profile_)->AddObserver(this);

  // TODO(crbug.com/1192394): multiple WebUI pages depend on the theme source
  // without adding it themselves. This is not causing an issue because the
  // theme source is being added here. The source should be added where it is
  // used and then the following can be removed.
  content::URLDataSource::Add(profile_,
                              std::make_unique<ThemeSource>(profile_));

  // Set up the data sources that Instant uses on the NTP.
  content::URLDataSource::Add(
      profile_, std::make_unique<FaviconSource>(
                    profile_, chrome::FaviconUrlFormat::kFaviconLegacy));
  content::URLDataSource::Add(profile_,
                              std::make_unique<MostVisitedIframeSource>());
  content::URLDataSource::Add(profile_,
                              std::make_unique<NewTabPageSource>());

  theme_observation_.Observe(native_theme_);

  if (custom_background_service_)
    custom_background_service_observation_.Observe(custom_background_service_);
}

InstantService::~InstantService() = default;

void InstantService::AddInstantProcess(int process_id) {
  process_ids_.insert(process_id);
}

bool InstantService::IsInstantProcess(int process_id) const {
  return process_ids_.find(process_id) != process_ids_.end();
}

void InstantService::AddObserver(InstantServiceObserver* observer) {
  observers_.AddObserver(observer);
}

void InstantService::RemoveObserver(InstantServiceObserver* observer) {
  observers_.RemoveObserver(observer);
}

void InstantService::OnNewTabPageOpened() {
  LOG(INFO) << "[Kiwi] InstantService::OnNewTabPageOpened: " << most_visited_sites_;
  if (most_visited_sites_) {
    most_visited_sites_->Refresh();
    most_visited_sites_->RefreshTiles();
  }
}

void InstantService::OnThemeChanged() {
  theme_ = nullptr;
  UpdateNtpTheme();
}

void InstantService::DeleteMostVisitedItem(const GURL& url) {
  if (most_visited_sites_) {
    most_visited_sites_->AddOrRemoveBlockedUrl(url, true);
  }
}

void InstantService::UndoMostVisitedDeletion(const GURL& url) {
  if (most_visited_sites_) {
    most_visited_sites_->AddOrRemoveBlockedUrl(url, false);
  }
}

void InstantService::UndoAllMostVisitedDeletions() {
  if (most_visited_sites_) {
    most_visited_sites_->ClearBlockedUrls();
  }
}

void InstantService::UpdateNtpTheme() {
  ApplyOrResetCustomBackgroundNtpTheme();
  SetNtpElementsNtpTheme();

  NotifyAboutNtpTheme();
}

void InstantService::UpdateMostVisitedInfo() {
  NotifyAboutMostVisitedInfo();
}

void InstantService::ResetCustomBackgroundInfo() {
  if (custom_background_service_) {
    custom_background_service_->ResetCustomBackgroundInfo();
  }
}

void InstantService::SetCustomBackgroundInfo(
    const GURL& background_url,
    const std::string& attribution_line_1,
    const std::string& attribution_line_2,
    const GURL& action_url,
    const std::string& collection_id) {
  if (custom_background_service_) {
    custom_background_service_->SetCustomBackgroundInfo(
        background_url, attribution_line_1, attribution_line_2, action_url,
        collection_id);
  }
}

void InstantService::SelectLocalBackgroundImage(const base::FilePath& path) {
  if (custom_background_service_) {
    custom_background_service_->SelectLocalBackgroundImage(path);
  }
}

NtpTheme* InstantService::GetInitializedNtpTheme() {
  if (custom_background_service_) {
    custom_background_service_->RefreshBackgroundIfNeeded();
  }

  if (!theme_)
    BuildNtpTheme();
  return theme_.get();
}

void InstantService::SetNativeThemeForTesting(ui::NativeTheme* theme) {
  theme_observation_.Reset();
  native_theme_ = theme;
  theme_observation_.Observe(native_theme_);
}

void InstantService::Shutdown() {
  process_ids_.clear();

  if (most_visited_sites_) {
    most_visited_sites_.reset();
  }

  ThemeServiceFactory::GetForProfile(profile_)->RemoveObserver(this);
}

void InstantService::OnCustomBackgroundImageUpdated() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  UpdateNtpTheme();
}

void InstantService::OnNtpCustomBackgroundServiceShuttingDown() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  custom_background_service_observation_.Reset();
  custom_background_service_ = nullptr;
}

void InstantService::Observe(int type,
                             const content::NotificationSource& source,
                             const content::NotificationDetails& details) {
  switch (type) {
    case content::NOTIFICATION_RENDERER_PROCESS_CREATED: {
      break;
    }
    case content::NOTIFICATION_RENDERER_PROCESS_TERMINATED: {
      content::RenderProcessHost* rph =
          content::Source<content::RenderProcessHost>(source).ptr();
      Profile* renderer_profile =
          static_cast<Profile*>(rph->GetBrowserContext());
      if (profile_ == renderer_profile)
        OnRendererProcessTerminated(rph->GetID());
      break;
    }
    default:
      NOTREACHED() << "Unexpected notification type in InstantService.";
  }
}

void InstantService::OnRendererProcessTerminated(int process_id) {
  process_ids_.erase(process_id);
}

void InstantService::OnNativeThemeUpdated(ui::NativeTheme* observed_theme) {
  DCHECK_EQ(observed_theme, native_theme_);
  // Force the theme information to rebuild so the correct using_dark_colors
  // value is sent to the renderer.
  BuildNtpTheme();
  UpdateNtpTheme();
}

void InstantService::OnURLsAvailable(
    const std::map<ntp_tiles::SectionType, ntp_tiles::NTPTilesVector>&
        sections) {
  DCHECK(most_visited_sites_);
  most_visited_info_->items.clear();
  // Use only personalized tiles for instant service.
  const ntp_tiles::NTPTilesVector& tiles =
      sections.at(ntp_tiles::SectionType::PERSONALIZED);
  LOG(INFO) << "[Kiwi] InstantService::OnURLsAvailable - tiles: " << tiles.size();
  for (const ntp_tiles::NTPTile& tile : tiles) {
    InstantMostVisitedItem item;
    item.url = tile.url;
    item.title = tile.title;
    item.favicon = tile.favicon_url;
    item.source = tile.source;
    item.title_source = tile.title_source;
    item.data_generation_time = tile.data_generation_time;
    most_visited_info_->items.push_back(item);
  }

  NotifyAboutMostVisitedInfo();
}

void InstantService::OnIconMadeAvailable(const GURL& site_url) {}

void InstantService::NotifyAboutMostVisitedInfo() {
  LOG(INFO) << "[Kiwi] InstantService::NotifyAboutMostVisitedInfo";
  for (InstantServiceObserver& observer : observers_)
    observer.MostVisitedInfoChanged(*most_visited_info_);
}

void InstantService::NotifyAboutNtpTheme() {
  for (InstantServiceObserver& observer : observers_)
    observer.NtpThemeChanged(*theme_);
}

void InstantService::BuildNtpTheme() {
  // Get theme information from theme service.
  theme_ = std::make_unique<NtpTheme>();

  // Get if the current theme is the default theme.
  ThemeService* theme_service = ThemeServiceFactory::GetForProfile(profile_);
  theme_->using_default_theme = theme_service->UsingDefaultTheme();

  // Get theme colors.
  const ui::ThemeProvider& theme_provider =
      ThemeService::GetThemeProviderForProfile(profile_);

  // Set colors.
  theme_->background_color =
      theme_provider.GetColor(ThemeProperties::COLOR_NTP_BACKGROUND);
  theme_->text_color_light =
      theme_provider.GetColor(ThemeProperties::COLOR_NTP_TEXT_LIGHT);
  SetNtpElementsNtpTheme();

  if (theme_service->UsingExtensionTheme()) {
    const extensions::Extension* extension =
        extensions::ExtensionRegistry::Get(profile_)
            ->enabled_extensions()
            .GetByID(theme_service->GetThemeID());
    if (extension) {
      theme_->theme_id = theme_service->GetThemeID();

      if (theme_provider.HasCustomImage(IDR_THEME_NTP_BACKGROUND)) {
        theme_->has_theme_image = true;

        // Set theme background image horizontal alignment.
        int alignment = theme_provider.GetDisplayProperty(
            ThemeProperties::NTP_BACKGROUND_ALIGNMENT);
        if (alignment & ThemeProperties::ALIGN_LEFT)
          theme_->image_horizontal_alignment = THEME_BKGRND_IMAGE_ALIGN_LEFT;
        else if (alignment & ThemeProperties::ALIGN_RIGHT)
          theme_->image_horizontal_alignment = THEME_BKGRND_IMAGE_ALIGN_RIGHT;
        else
          theme_->image_horizontal_alignment = THEME_BKGRND_IMAGE_ALIGN_CENTER;

        // Set theme background image vertical alignment.
        if (alignment & ThemeProperties::ALIGN_TOP)
          theme_->image_vertical_alignment = THEME_BKGRND_IMAGE_ALIGN_TOP;
        else if (alignment & ThemeProperties::ALIGN_BOTTOM)
          theme_->image_vertical_alignment = THEME_BKGRND_IMAGE_ALIGN_BOTTOM;
        else
          theme_->image_vertical_alignment = THEME_BKGRND_IMAGE_ALIGN_CENTER;

        // Set theme background image tiling.
        int tiling = theme_provider.GetDisplayProperty(
            ThemeProperties::NTP_BACKGROUND_TILING);
        switch (tiling) {
          case ThemeProperties::NO_REPEAT:
            theme_->image_tiling = THEME_BKGRND_IMAGE_NO_REPEAT;
            break;
          case ThemeProperties::REPEAT_X:
            theme_->image_tiling = THEME_BKGRND_IMAGE_REPEAT_X;
            break;
          case ThemeProperties::REPEAT_Y:
            theme_->image_tiling = THEME_BKGRND_IMAGE_REPEAT_Y;
            break;
          case ThemeProperties::REPEAT:
            theme_->image_tiling = THEME_BKGRND_IMAGE_REPEAT;
            break;
        }

        theme_->has_attribution =
            theme_provider.HasCustomImage(IDR_THEME_NTP_ATTRIBUTION);
      }
    }
  }
}

void InstantService::ApplyOrResetCustomBackgroundNtpTheme() {
  // Custom backgrounds for non-Google search providers are not supported.
  if (!search::DefaultSearchProviderIsGoogle(profile_)) {
    ResetCustomBackgroundNtpTheme();
    return;
  }

  auto custom_background =
      custom_background_service_
          ? custom_background_service_->GetCustomBackground()
          : absl::optional<CustomBackground>();

  if (!custom_background) {
    ResetCustomBackgroundNtpTheme();
    return;
  }

  GetInitializedNtpTheme()->custom_background_url =
      custom_background->custom_background_url;
  GetInitializedNtpTheme()->custom_background_attribution_line_1 =
      custom_background->custom_background_attribution_line_1;
  GetInitializedNtpTheme()->custom_background_attribution_line_2 =
      custom_background->custom_background_attribution_line_2;
  GetInitializedNtpTheme()->custom_background_attribution_action_url =
      custom_background->custom_background_attribution_action_url;
  GetInitializedNtpTheme()->collection_id = custom_background->collection_id;
}

void InstantService::ResetCustomBackgroundNtpTheme() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (custom_background_service_) {
    custom_background_service_->ResetCustomBackgroundNtpTheme();
  }
  FallbackToDefaultNtpTheme();
}

void InstantService::FallbackToDefaultNtpTheme() {
  NtpTheme* theme = GetInitializedNtpTheme();
  theme->custom_background_url = GURL();
  theme->custom_background_attribution_line_1 = std::string();
  theme->custom_background_attribution_line_2 = std::string();
  theme->custom_background_attribution_action_url = GURL();
  theme->collection_id = std::string();
}

bool InstantService::IsCustomBackgroundDisabledByPolicy() {
  return custom_background_service_ &&
         custom_background_service_->IsCustomBackgroundDisabledByPolicy();
}

bool InstantService::IsCustomBackgroundSet() {
  return custom_background_service_ &&
         custom_background_service_->IsCustomBackgroundSet();
}

void InstantService::ResetToDefault() {
  ResetCustomBackgroundNtpTheme();
}

void InstantService::AddValidBackdropUrlForTesting(const GURL& url) const {
  custom_background_service_->AddValidBackdropUrlForTesting(url);
}

void InstantService::AddValidBackdropCollectionForTesting(
    const std::string& collection_id) const {
  custom_background_service_->AddValidBackdropCollectionForTesting(
      collection_id);
}

void InstantService::SetNextCollectionImageForTesting(
    const CollectionImage& image) const {
  custom_background_service_->SetNextCollectionImageForTesting(image);
}

// static
void InstantService::RegisterProfilePrefs(PrefRegistrySimple* registry) {
  NtpCustomBackgroundService::RegisterProfilePrefs(registry);
}

// static
bool InstantService::ShouldServiceRequest(
    const GURL& url,
    content::BrowserContext* browser_context,
    int render_process_id) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  auto* instant_service = InstantServiceFactory::GetForProfile(
      static_cast<Profile*>(browser_context));

  if (!instant_service)
    return false;

  // The process_id for the navigation request will be -1. If
  // so, allow this request since it's not going to another renderer.
  return render_process_id == -1 ||
         instant_service->IsInstantProcess(render_process_id);
}

void InstantService::SetClockForTesting(base::Clock* clock) {
  custom_background_service_->SetClockForTesting(clock);
}

void InstantService::SetNtpElementsNtpTheme() {
  NtpTheme* theme = GetInitializedNtpTheme();
  if (IsCustomBackgroundSet()) {
    theme->text_color = gfx::kGoogleGrey050;
    theme->logo_alternate = true;
  } else {
    const ui::ThemeProvider& theme_provider =
        ThemeService::GetThemeProviderForProfile(profile_);
    theme->text_color =
        theme_provider.GetColor(ThemeProperties::COLOR_NTP_TEXT);
    theme->logo_alternate = theme_provider.GetDisplayProperty(
                                ThemeProperties::NTP_LOGO_ALTERNATE) == 1;
  }
}
