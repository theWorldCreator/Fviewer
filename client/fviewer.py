#!/usr/bin/python
# coding: utf-8

import threading
#import sys
import os, re, json
import socket, time, gobject
from datetime import datetime
import gtk
import gst
import webkit
from urllib import urlopen
from random import randrange


have_appindicator = True
try:
	import appindicator
except:
	have_appindicator = False

gobject.threads_init()


__fviewer_data_directory__ = 'data/'

def get_data_file(*path_segments):
	"""Get the full path to a data file."""
	return os.path.join(get_data_path(), *path_segments)


def get_data_path():
	path = os.path.join(
		os.path.dirname(__file__), __fviewer_data_directory__)

	abs_data_path = os.path.abspath(path)
	if not os.path.exists(abs_data_path):
		raise project_path_not_found

	return abs_data_path



if os.path.isfile(get_data_file("", ".settings.py")):
	exec compile(open(get_data_file("", ".settings.py"), "r").read(), "client_settings", 'exec')
else:
	# Default settings
	class Settings:
		def __init__(self):
			self.min_money = 0
			self.max_money = 0				# 0 means, that there no upper limit
			self.without_money = 1			# Client accept projects without specified budget
			self.categories = range(0, 17)
			self.search_string = ''
			self.money_type = 2				# Currency, 2 means rubbles
			self.new_window = 1				# Open new projects in new window
			self.audio_notification = 1
			self.audio_notification_level = 0.5	# Half volume
settings = Settings()

categories_list = [u'Дизайн', u'Программирование', u'Веб-строй', u'Раскрутка', u'Тексты и переводы', u'Верстка', u'Flash', u'Логотипы', u'Иллюстрации', u'3D', u'Аудио/Видео', u'Иконки', u'Разное', u'Фото', u'Консалтинг', u'Маркетинг', u'Администрирование']
categories_dict = {}
i = 0
for category in categories_list:
	categories_dict[category] = i
	i += 1
money_types = [u'$', u'€', u'руб', u'FM']
# ToDo: get this values from the internet
int_money_coefficient = [28.06, 40.24, 1, 30.0]		# FM is internal currency for free-lance.ru

class Projects:
	def __init__(self):
		self.current_id = -1
		self.current_real_id = -1
		self.unread_count = 0
		self.count = 0
		self.matched = []
	
	def get(self, pr_id):
		print pr_id
		if pr_id == -1 or self.matched[pr_id] == -1:
			return None
		fv_id = self.matched[pr_id]['project_id']
		project = fviewer.projects[fv_id]
		project.update(self.matched[pr_id])
		return project
		
	def set(self, pr_id):
		self.current_id = pr_id
		project = self.current()
		if project is None:
			return None
		self.current_real_id = project['id']
		if project['read'] == 0:
			#fviewer.unread_projects_count -= 1
			self.unread_count -= 1
			fviewer.projects[project['project_id']]['read'] = 1
	
	def current(self):
		return self.get(self.current_id)
	
	def update(self):
		# Update current_id
		i = 0
		while i < fviewer.projects_count and fviewer.projects[i]['id'] != self.current_real_id:
			i += 1
		if i < fviewer.projects_count:
			current_fviewer_id = i
		else:
			current_fviewer_id = -1
		
		
		self.unread_count = 0
		self.current_id = -1
		self.count = 0
		self.matched = []
		for i in range(fviewer.projects_count):
			project = fviewer.projects[i]
			if design.now_search_text != '':
				if unicode(project['title']).lower().find(design.now_search_text) == -1 and unicode(project['description']).lower().find(design.now_search_text) == -1:
					continue
			if not project['read']:
				self.unread_count += 1
			self.count += 1
			self.matched.append({'project_id': i})
			
			if current_fviewer_id == i:
				self.current_id = self.count - 1
				design.update_status_bar(self.current_id)
		
		# Make "read" mark for outdated projects
		if self.count > design.max_projects:
			for i in range(self.count - design.max_projects):
				if self.get(i)['read'] == 0:
					fviewer.projects[self.get(i)['project_id']]['read'] = 1
					#fviewer.unread_projects_count -= 1
					self.unread_count -= 1
	
	
	def button_id(self, index):
		"""
		If project with id = index is one of the last self.max_projects projects => return index of button, else return negative value
		"""
		if index < 0:
			return -1
		if self.count >= design.max_projects:
			return index - self.count + design.max_projects
		return index
			
	def first_unread_id(self):
		if self.unread_count == 0:
			# Let's show first project list item
			if self.count >= design.max_projects:
				return self.count - design.max_projects
			elif self.count > 0:
				return 0
			else:
				return -1
		i = 0
		while i < self.count and self.get(i)['read']:
			i += 1
		return i


class FviewerDesign:
	def __init__(self):
		# settings
		self.max_projects = 10			# How many projects are shown to user in te list
		self.X, self.Y = 300, 100
		self.MAX_LINK_LEN = 50
		self.MAX_TITLE_LEN = 53
		self.MAX_BOLD_TITLE_LEN = 50
		self.MAX_LARGE_TITLE_LEN = 44
		
		
		self.builder = gtk.Builder()
		self.builder.add_from_file(get_data_file('ui', 'MainWindow.ui'))
		self.main_window = self.builder.get_object("MainWindow")
		self.status_bar = self.builder.get_object("Statusbar")
		self.project_desc_text_buffer = self.builder.get_object("ProjectDescription")
		
		vbox = self.builder.get_object("ProjectsListVbox")
		self.projects_list_buttons = {}
		for i in range(self.max_projects):
			self.projects_list_buttons[i] = {}
			event = gtk.EventBox()
			obj = self.projects_list_buttons[i][0] = gtk.Label('')
			event.add(obj)
			event.connect('button-press-event', self.open_project_from_list, i)
			vbox.pack_start(event)
			obj.show()
			event.show()
		self.project_desc_html = webkit.WebView()
		self.builder.get_object('DescriptionScrollWind').add(self.project_desc_html)
		self.project_desc_html.connect('key-press-event', self.hot_key)
		self.project_desc_html.zoom_out()
		self.project_desc_html.show()
		self.builder.get_object("SearchEntry").set_text(settings.search_string)
		
		
		
		dic = { "search_projects" : self.update_projects_list,
			"hot_key" : self.hot_key,
			"projects_list_scroll" : self.projects_list_scroll,
			"main_window_close" : self.main_window_close }
		self.builder.connect_signals(dic)
		
		
		ProjectsDialog = gtk.Builder()
		ProjectsDialog.add_from_file(get_data_file('ui', 'ProjectsDialog.ui'))
		dic = { "dialog_response" : self.dialog_response,
				"window_close": lambda w: self.dialog.response(2)}
		ProjectsDialog.connect_signals(dic)
		
		SettingsWindow = gtk.Builder()
		SettingsWindow.add_from_file(get_data_file('ui', 'SettingsWindow.ui'))
		dic = { "money_type_changed" : self.money_type_changed,
			"audio_notification_button_toggled" : lambda x: self.volume_button.set_sensitive(self.audio_notification_button.get_active()),
			"settings_window_close" : self.settings_window_close,
			"save_settings" : self.save_settings,
			"undo_settings" : self.undo_settings }
		SettingsWindow.connect_signals(dic)
		
		SettingsDialog = gtk.Builder()
		SettingsDialog.add_from_file(get_data_file('ui', 'SettingsDialog.ui'))
		dic = { "settings_dialog_response" : self.settings_dialog_response,
			"window_close" : lambda x: self.settings_dialog.response(1) }
		SettingsDialog.connect_signals(dic)
		
		self.dialog = ProjectsDialog.get_object('dialog')
		self.remember_dialog_action = ProjectsDialog.get_object('checkbutton')
		
		
		self.settings_dialog = SettingsDialog.get_object('settings_dialog')
		
		self.settings_window = SettingsWindow.get_object('settings_window')
		self.minimum_money_entry = SettingsWindow.get_object('minimum_money_entry')
		self.maximum_money_entry = SettingsWindow.get_object('maximum_money_entry')
		self.without_money_widget = SettingsWindow.get_object('without_money_widget')
		# ComboBox for choosing currency which I couldn't set from glade
		liststore = gtk.ListStore(str)
		self.money_type_widget = SettingsWindow.get_object('MoneyType')
		self.money_type_widget.set_model(liststore)
		cell = gtk.CellRendererText()
		self.money_type_widget.pack_start(cell, True)
		self.money_type_widget.add_attribute(cell, 'text', 0)
		self.money_type_widget.append_text('USD')
		self.money_type_widget.append_text('Euro')
		self.money_type_widget.append_text('Руб')
		self.money_type_widget.append_text('FM')
		self.money_type_widget.set_active(0)
		
		
		table = SettingsWindow.get_object('categories_table')
		a = 1
		i = 0
		self.categories_check_buttons = {}
		for category, b in categories_dict.items():
			widg = gtk.CheckButton(category)
			widg.set_active(1)
			if i%2 == 0:
				table.attach(widg, 1, 2, a, a+1)
			else:
				table.attach(widg, 2, 3, a, a+1)
				a += 1
			self.categories_check_buttons[b] = widg
			widg.show()
			i += 1
		self.new_window_checkbutton = SettingsWindow.get_object('new_window_checkbutton')
		self.audio_notification_button = SettingsWindow.get_object('audio_notification_button')
		# Volume button, you will not see icon if add it from glade
		self.volume_button = gtk.VolumeButton()
		self.volume_button.show()
		SettingsWindow.get_object('audio_hbox').pack_start(self.volume_button)
		
		
		# Menu
		menu = gtk.Menu()
		# Settings menu item
		menu_item = gtk.MenuItem('Настройки')
		menu_item.connect('activate', lambda x: self.settings_window.present())
		menu.append(menu_item)
		
		# List of project menu item
		menu_item = gtk.MenuItem('Список проектов')
		menu_item.connect('activate', self.show_projects_list)
		menu.append(menu_item)
		
		# Pause menu item
		menu_item = gtk.CheckMenuItem('Пауза')
		menu_item.connect('toggled', self.pause)
		menu.append(menu_item)
		
		separator = gtk.MenuItem()
		menu.append(separator)

		menu_quit = gtk.ImageMenuItem(gtk.STOCK_QUIT)
		menu_quit.connect('activate', self.quit)
		menu.append(menu_quit)
		
		menu.show_all()
		
		# Tray icon
		self.tray_iconbuf = gtk.gdk.pixbuf_new_from_file(get_data_file("media", "icon.png"))
		self.tray_pause_iconbuf = gtk.gdk.pixbuf_new_from_file(get_data_file("media", "tray_pause.png"))
		
		# Tray
		if have_appindicator:
			self.tray = appindicator.Indicator("Fviewer", "fviewer", appindicator.CATEGORY_APPLICATION_STATUS)
			self.tray.set_status (appindicator.STATUS_ACTIVE)
			self.tray.set_icon(get_data_file("media", "tray_pause.png"))
			self.tray.set_menu(menu)
		else:
			self.tray = gtk.StatusIcon()
			
			self.tray.set_from_pixbuf(self.tray_pause_iconbuf)
			# "No connection" tooltip
			self.tray.set_tooltip('Нет соединения')
			self.tray.connect('popup-menu', self.menu_popup, menu)
			self.tray.connect('activate', self.activate_tray)
		
		self.make_icons_grey()
		
		
		self.first_run = True
		self.unread_projects_icon = False
		self.main_window_first_present = True
		self.now_paused = False
		self.dialog_last_answer = 0
		text = self.builder.get_object("SearchEntry").get_text()
		self.now_search_text = unicode(text).lower()
		self.playbin = gst.element_factory_make("playbin2", "my-playbin")	# For playing audio alerts
		self.playbin.set_property("uri", "file://" + get_data_file("media", "sound.wav"))
		
		
		
		# updating settings window
		self.undo_settings(0)
	
	def projects_list_scroll(self, widget, scroll_type):
		if scroll_type.direction == gtk.gdk.SCROLL_UP:
			self.to_next_previous_project('', -1)
		elif scroll_type.direction == gtk.gdk.SCROLL_DOWN:
			self.to_next_previous_project('', 1)
	
	def hot_key(self, widget, key):
		if key.keyval == 65363:
			# right button
			self.to_next_previous_project('', 1)
		if key.keyval == 65361:
			# left button
			self.to_next_previous_project('', -1)
		
	def update_status_bar(self, project_id):
		project = projects.get(project_id)#fviewer.projects[self.matched_projects[project_id]['project_id']]
		now_id = project['project_id'] + 1
		if now_id >= 1:
			now_id = str(now_id)
		else:
			now_id = '-'
		
		st_bar_text = project['status_str'] + "     " + now_id + '/' + str(fviewer.projects_count)
		self.status_bar.push(self.status_bar.get_context_id(st_bar_text), st_bar_text)
		
	def change_project_area(self, text, title, link):
		#self.main_window.move(self.X, self.Y)
		self.builder.get_object("LinkButton").set_uri(link)
		if len(link) > self.MAX_LINK_LEN:
			link = link[:self.MAX_LINK_LEN - 3] + "..."
		self.builder.get_object("LinkButton").set_label(link)
		self.main_window.set_title(title)
		self.project_desc_html.load_string(text, "text/html", "utf-8", "")
	
	
	def show_project(self, index):
		# Un-italic or un-bold previous title
		if projects.button_id(projects.current_id) >= 0:
			title = projects.current()['title']
			if len(title) > self.MAX_TITLE_LEN:
				title = title[:self.MAX_TITLE_LEN - 3] + "..."
			self.projects_list_buttons[projects.button_id(projects.current_id)][0].set_label(get_usual_title(title))
		
		projects.set(index)
		project = projects.current()
		if project is None:
			return
		project_id = project['project_id']
			
		# Mark current title
		if projects.button_id(index) >= 0:
			# If project with id = index is one of the last self.max_projects projects
			self.projects_list_buttons[projects.button_id(index)][0].set_markup(get_current_title(project['title']))
				
		self.change_icon()
		self.change_project_area(project["description"], project["title"], project["link"])
		self.update_status_bar(index)
		self.project_desc_html.grab_focus()
	
	
	def play_audio_notification(self):
		if settings.audio_notification:
			self.playbin.set_property("volume", settings.audio_notification_level)
			self.playbin.set_state(gst.STATE_NULL)
			self.playbin.set_state(gst.STATE_PLAYING)
		
	def change_icon(self):
		if self.unread_projects_icon == False and projects.unread_count > 0:
			if fviewer.quit == 0:
				#if sys.platform == 'win32':
				#	self.tray.set_from_pixbuf(self.tray_unread_iconbuf)
				#else:
				if have_appindicator:
					self.tray.set_icon('mail-unread')
				else:
					self.tray.set_from_icon_name('mail-unread')
			self.unread_projects_icon = True
		if self.unread_projects_icon == True and projects.unread_count == 0:
			self.unread_projects_icon = False
			if fviewer.quit == 0:
				if have_appindicator:
					self.tray.set_icon(get_data_file("media", "icon.png"))
				else:
					self.tray.set_from_pixbuf(self.tray_iconbuf)
		
	def show_next_unread_project(self, open_anyway = False):
		if (open_anyway or settings.new_window == 1) and projects.unread_count > 0 and self.main_window.get_property('visible') == False and self.dialog.get_property('visible') == False:
			self.show_project(projects.first_unread_id())
			self.present_main_window()
	
	def open_project_from_list(self, widget, key, i):
		self.X, self.Y = self.main_window.get_position()
		self.show_project(self.projects_list_buttons[i][1])
	
	def to_next_previous_project(self, widget, direction):
		if projects.current_id > 0 and direction == -1:
			self.X, self.Y = self.main_window.get_position()
			self.show_project(projects.current_id - 1)
		if projects.current_id < (projects.count - 1) and direction == 1:
			self.X, self.Y = self.main_window.get_position()
			self.show_project(projects.current_id + 1)
	
	
	def settings_window_save(self):
		if not is_int(self.minimum_money_entry.get_text()):
			self.minimum_money_entry.set_text('')
		if not is_int(self.maximum_money_entry.get_text()):
			self.maximum_money_entry.set_text('')
		if settings.money_type != self.money_type_widget.get_active():	# Currency was changed
			settings.money_type = self.money_type_widget.get_active()
			# Updating status string for each project
			for i in range(fviewer.projects_count):
				fviewer.projects[i]['status_str'] = get_project_status_str(fviewer.projects[i])
			# Uodating string in status bar for current project
			if projects.current_id >= 0:
				self.update_status_bar(projects.current_id)
		status = self.settings_window_changed()
		if status > 0:
			settings.min_money = to_another_money_type(self.minimum_money_entry.get_text(), settings.money_type, 2)
			if settings.min_money == 0:
				self.minimum_money_entry.set_text('')
			settings.max_money = to_another_money_type(self.maximum_money_entry.get_text(), settings.money_type, 2)
			if settings.max_money == 0:
				self.maximum_money_entry.set_text('')
			settings.without_money = int(self.without_money_widget.get_active())
			settings.categories = []
			for i, check_button in self.categories_check_buttons.items():
				if check_button.get_active(): settings.categories.append(i)
			
			settings.new_window = self.new_window_checkbutton.get_active()
			settings.audio_notification = self.audio_notification_button.get_active()
			settings.audio_notification_level = self.volume_button.get_value()
			
			if status == 1:
				fviewer.id = "-1"
		
		# Save new settings
		open(get_data_file("", ".settings.py"), 'w').write( settings_file_content() )
		
	def settings_window_changed(self):
		"""
		Return 1 or 2 if settings was changed and 0 if not
		"""
		if not merge_money(self.minimum_money_entry.get_text(), self.money_type_widget.get_active(), settings.min_money, 2):
			return 1
		if not merge_money(self.maximum_money_entry.get_text(), self.money_type_widget.get_active(), settings.max_money, 2):
			return 1
		if self.without_money_widget.get_active() != settings.without_money:
			return 1
		
		for i, check_button in self.categories_check_buttons.items():
			if (i in settings.categories) and check_button.get_active() == 0:
				return 1
			if (i not in settings.categories) and check_button.get_active() == 1:
				return 1
		
		if self.new_window_checkbutton.get_active() != settings.new_window:
			return 2
		if self.audio_notification_button.get_active() != settings.audio_notification:
			return 2
		if self.volume_button.get_value() != settings.audio_notification_level:
			return 2
		return 0
	
	
	def update_projects_list(self, widget = None, key = 0):
		if key != 0:
			if self.builder.get_object("SearchEntry").get_text() != self.now_search_text:
				text = self.builder.get_object("SearchEntry").get_text()
				self.now_search_text = unicode(text).lower()
			else:
				return False
		
		projects.update()
		
		a = 0
		if projects.count < self.max_projects:
			start = 0
		else:
			start = projects.count - self.max_projects
		for ind in range(start, projects.count):
			project = projects.get(ind)
			#projects.matched[ind]['button_id'] = a
			title = project['title']
			if project['read'] == 1:
				if ind == projects.current_id:
					self.projects_list_buttons[a][0].set_markup(get_current_title(title))
				else:
					self.projects_list_buttons[a][0].set_markup(get_usual_title(title))
			else:
				self.projects_list_buttons[a][0].set_markup(get_unread_title(title))
			
			self.projects_list_buttons[a][1] = ind
			self.projects_list_buttons[a][0].show()
			a += 1
		if projects.count < self.max_projects:
			if projects.count == 0:
				self.builder.get_object("ProjectsListNothingSearched").show()
			for i in range(projects.count, self.max_projects):
				self.projects_list_buttons[i][0].hide()
		if projects.count > 0:
			self.builder.get_object("ProjectsListNothingSearched").hide()
	
	
	def show_projects_list(self, widget = None):
		self.update_projects_list(widget)
		self.present_main_window()
		if self.main_window.get_property('visible') == False:
			self.main_window.move(self.X, self.Y)
	
	
	def update_from_core_thread(self):
		projects.update()
		
		self.change_icon()
		self.internet_problems(problems = False)
		
		# Present main window, if necessary
		if settings.new_window:
			if self.main_window.get_property('visible'):
				self.show_projects_list()
			else:
				self.update_projects_list()
			self.show_next_unread_project()
		else:
			self.update_projects_list()
			if self.first_run:
				# If client do not main window to be opened than new projects appears, let's set project which he sees as open main window manually
				self.show_project(projects.first_unread_id())
		if self.first_run:
			self.first_run = False
	
	def present_main_window(self):
		if not self.main_window_first_present:
			self.main_window.present()
		else:
			# Set default list width
			title = self.projects_list_buttons[0][0].get_text()
			visible = self.projects_list_buttons[0][0].get_property('visible')
			# Set very long fish title text and projects list will automatically resized 
			self.projects_list_buttons[0][0].set_markup(get_unread_title(u"Пример очень очень очень очень длинного заголовка"))
			if not visible:
				self.projects_list_buttons[0][0].show()
			self.main_window.present()
			self.projects_list_buttons[0][0].set_markup(title)
			if not visible:
				self.projects_list_buttons[0][0].hide()
			self.main_window_first_present = False
		
	
	
	def make_icons_grey(self, make_grey = True):
		if make_grey:
			if have_appindicator:
				self.tray.set_icon(get_data_file("media", "tray_pause.png"))
			else:
				self.tray.set_from_pixbuf(self.tray_pause_iconbuf)
			self.main_window.set_icon(self.tray_pause_iconbuf)
			self.settings_window.set_icon(self.tray_pause_iconbuf)
		else:
			self.unread_projects_icon = not self.unread_projects_icon
			self.main_window.set_icon(self.tray_iconbuf)
			self.settings_window.set_icon(self.tray_iconbuf)
			self.change_icon()
			if not have_appindicator:
				self.tray.set_tooltip('FViewer')
	
	def internet_problems(self, problems = True):
		"""
		problems is True means losing internet connection
		problems is False means finding it
		"""
		if problems and fviewer.internet_problems == 0:
			if not have_appindicator:
				# "No connection" tooltip
				self.tray.set_tooltip('Нет соединения')
			fviewer.internet_problems = 1
			self.make_icons_grey(True)
		if not problems and fviewer.internet_problems == 1:
			fviewer.internet_problems = 0
			self.make_icons_grey(False)
	
	def money_type_changed(self, widget):
		"""
		Connected with currency changing signal
		"""
		money_type_to = self.money_type_widget.get_active()
		min_money = self.minimum_money_entry.get_text()
		if min_money != '':
			self.minimum_money_entry.set_text(str(to_another_money_type(min_money, self.last_money_type_value, money_type_to)))
		max_money = self.maximum_money_entry.get_text()
		if max_money != '':
			self.maximum_money_entry.set_text(str(to_another_money_type(max_money, self.last_money_type_value, money_type_to)))
		self.last_money_type_value = money_type_to
		
		
	def activate_tray(self, widget):
		"""
		Connected with click on tray icon
		"""
		if self.unread_projects_icon == False or fviewer.quit == 1:
			if self.main_window.get_property('visible'):
				self.X, self.Y = self.main_window.get_position()
				self.main_window.hide()
			else:
				self.show_projects_list()
		else:
			if self.main_window.get_property('visible'):
				self.X, self.Y = self.main_window.get_position()
				self.main_window.hide()
			else:
				self.show_next_unread_project(True)
				self.main_window.move(self.X, self.Y)
	
	def show_filters_window(self, widget):
		self.filters_window.present()
	
	
	def settings_window_close(self, widget, b):
		if self.settings_window_changed() > 0:
			self.settings_dialog.present()
		else:
			self.settings_window.hide()
		return True
	
	def settings_dialog_response(self, dialog, response_id):
		dialog.hide()
		if response_id == 2:
			self.undo_settings(0)
			self.settings_window.hide()
		if response_id == 3:
			self.save_settings(0)
	
	def save_settings(self, widget):
		self.settings_window_save()
		self.settings_window.hide()
	
	def dialog_response(self, dialog, response_id):
		if self.remember_dialog_action.get_active():
			self.dialog_last_answer = response_id
		dialog.hide()
		if response_id == 1:
			self.show_next_unread_project()
		if response_id == 2:
			for i in range(fviewer.projects_count):
				fviewer.projects[i]["read"] = 1
			#fviewer.unread_projects_count = 0
			projects.unread_count = 0
			self.change_icon()
		
	
	def pause(self, item):
		fviewer.quit = item.get_active()
		if fviewer.quit == 1:
			if not have_appindicator:
				# "Pause" tooltip
				self.tray.set_tooltip('Пауза')
			self.make_icons_grey(True)
		else:
			if fviewer.internet_problems == 1:
				if not have_appindicator:
					# "No connection" tooltip
					self.tray.set_tooltip('Нет соединения')
			if fviewer.internet_problems == 0:
				self.make_icons_grey(False)
	
	def undo_settings(self, widget):
		self.money_type_widget.set_active(settings.money_type)
		self.last_money_type_value = settings.money_type
		if settings.min_money > 0:
			self.minimum_money_entry.set_text(str(to_another_money_type(settings.min_money, 2, settings.money_type)))
		else:
			self.minimum_money_entry.set_text('')
		if settings.max_money > 0:
			self.maximum_money_entry.set_text(str(to_another_money_type(settings.max_money, 2, settings.money_type)))
		else:
			self.maximum_money_entry.set_text('')
		self.without_money_widget.set_active(settings.without_money)
		for i, check_button in self.categories_check_buttons.items():
			if i in settings.categories:
				check_button.set_active(1)
			else:
				check_button.set_active(0)
		
		self.new_window_checkbutton.set_active(settings.new_window)
		self.audio_notification_button.set_active(settings.audio_notification)
		self.volume_button.set_sensitive(settings.audio_notification)
		self.volume_button.set_value(settings.audio_notification_level)
		return True
	
		
	def menu_popup(self, widget, button, time, data = None):
			if button==3 and data:
					data.show_all()
					data.popup(None, None, None, 3, time)

	def main_window_close(self, widget, event, suggest_dialog = True):
		self.X, self.Y = self.main_window.get_position()
		self.main_window.hide()
		if suggest_dialog and settings.new_window == 1 and projects.unread_count > 0:
			if self.dialog_last_answer == 0:
				self.dialog.present()
			else:
				self.dialog_response(self.dialog, self.dialog_last_answer)
		return True
	
	
	def quit(self,widget):
		fviewer.quit = 2
		if have_appindicator:
			self.tray.set_status(appindicator.STATUS_PASSIVE)
		else:
			self.tray.set_visible(False)
		self.settings_window.hide()
		self.main_window.hide()
		self.dialog.hide()
		self.settings_dialog.hide()
		open(get_data_file("", ".settings.py"), 'w').write( settings_file_content() )
		gtk.main_quit()

class Fviewer(threading.Thread):
		def __init__(self):
			super(Fviewer, self).__init__()
			# Settings
			self.max_projects = 30			# How namy projects store in memory
			self.read_timeout = 5
			self.write_timeout = 10			# Timeout for sending request to server
			self.update_timeout = 10		# Delay between request for new projects
			self.reconnection_timeout = 20	# How many time wait until reconnection try
			
			self.last_project = -1
			self.last_project_id = -1
			self.id = -1
			
			self.HOST = "fviewer.info"
			self.PORT = 23143
			
			self.projects = []
			self.projects_count = 0
			self.internet_problems = 1		# No connection before we try to find it
			self.quit = 0					# 0 -- normal work, 1 - pause, 2 - quit
		
		def run(self):
			self.updater()
		
		def updater(self):
			"""
			Main loop for updating projects info
			"""
			while 1:
				if self.quit == 2:
					break
				if self.quit == 0:
					timeout = self.get_projects()
				time.sleep(timeout)
			
		
		def read_data(self, sock):
			data = ""
			now = time.time()
			answ = {}
			
			end = False
			while not end:
				try:
					# FIXME: What will happen, if there no data when you trying to recv()?
					tmp = sock.recv(1024)
				except:
					tmp = False
				if tmp:
					data += tmp
				
				if (time.time()-now) > self.read_timeout:
					return -1
				#print data
				try:
					answ = json.loads(data)
					end = True
				except ValueError:
					end = False
			
			if 'error' in answ:
				if answ['error'] == 'Wrong userid':
					self.id = -1
					return -1
				if answ['error'] == 'Wrong data':
					return -1
			
			print data
			return answ
			
			
		def authorization(self):
			self.id = -1
			self.hash = randrange(10000, 100000)
			
			sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
			try:
				sock.connect((self.HOST, self.PORT))
				sock.settimeout(self.write_timeout)
				sock.send(json.dumps({"hash": self.hash, "min_mon": settings.min_money, "max_mon": settings.max_money,
										"wth_mon": settings.without_money, "last_proj": self.last_project,
										"last_proj_id": self.last_project_id, "categ": settings.categories}))
			except:
				return False
			data = self.read_data(sock)
			sock.close()
			
			if data == -1:
				return False
			
			self.id = data["id"]
			
			return True
		
		
		def get_projects(self):
			"""
			Returns after that what delay you need to call this function again
			"""
			if self.id == -1:
				if not self.authorization():
					gobject.idle_add(design.internet_problems)
					return self.reconnection_timeout
			
			
			sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
			try:
				sock.connect((self.HOST, self.PORT))
				sock.settimeout(self.write_timeout)
				sock.send(json.dumps({"id": self.id, "hash": self.hash}))
			except:
				gobject.idle_add(design.internet_problems)
				return self.reconnection_timeout
			
			data = self.read_data(sock)
			sock.close()
			
			if data != -1 and len(data) > 0:
				added = 0
				for project in data:
					project['status_str'] = get_project_status_str(project)
					project['read'] = 0
					
					project['description'] = get_description(project['description'])
					
					self.projects.append(project)
					if self.projects_count >= self.max_projects:
						self.projects.pop(0)
					else:
						self.projects_count += 1
					added = 1
					self.last_project = project['loc_id']
					self.last_project_id = project['id']
				if added:
					gobject.idle_add(design.play_audio_notification)
					gobject.idle_add(design.update_from_core_thread)
			return self.update_timeout

def is_int( str ):
	ok = True
	try:
		num = int(str)
	except ValueError:
		ok = False
	return ok

def to_another_money_type(money, type_from, type_to):
	if money == '':
		money = 0
	if is_int(money):
		return int(float(money)*int_money_coefficient[type_from]/int_money_coefficient[type_to])
	else:
		return 0
		
def merge_money(money1, type1, money2, type2):
	"""
	True if money1 in type1 currency is equal to money2 in type2 currency
	"""
	if abs((to_another_money_type(money1, type1, 2) - to_another_money_type(money2, type2, 2))) > 5:	# 5 rubbles difference can arise because of rounding errors
		return False
	else:
		return True

def get_project_status_str(project):
	categories_count = len(project['categ'])
	project['status_str'] = ''
	if project['money'] > 0:
		# "Budget: ..."
		project['status_str'] += "Бюджет: " + str(to_another_money_type(project['money'], 2, settings.money_type)) + " " + money_types[settings.money_type] + "    "
	project['status_str'] += time.strftime("Добавленно в %I:%M", time.localtime(project['pubDate'])) + "    "
	if categories_count > 0:
		if categories_count == 1:
			# "Category: "
			project['status_str'] += "Категория: "
		else:
			# "Categories: "
			project['status_str'] += "Категории: "
		for cat in project['categ']:
			project['status_str'] += categories_list[cat] + ", "
		project['status_str'] = project['status_str'][:-2]
	return project['status_str']


def get_usual_title(title):
	if len(title) > design.MAX_TITLE_LEN:
		title = title[:design.MAX_TITLE_LEN - 3] + "..."
	return title

def get_current_title(title):
	if len(title) > design.MAX_LARGE_TITLE_LEN:
		title = title[:design.MAX_LARGE_TITLE_LEN - 3] + "..."
	return "<span size = 'larger'>" + title + "</span>"

def get_unread_title(title):
	if len(title) > design.MAX_BOLD_TITLE_LEN:
		title = title[:design.MAX_BOLD_TITLE_LEN - 3] + "..."
	return "<b>" + title + "</b>"

def get_description(text):
	# Some stuff to make project description look good in the WebKit
	return "<html><head><title></title></head><body>" + text.replace("\r\n", "<br />").replace("\n", "<br />").replace("\r", "<br />") + "</body></html>"

def settings_file_content():
	return """
class Settings:
	def __init__(self):
		self.min_money = """ + str(settings.min_money) + """
		self.max_money = """ + str(settings.max_money) + """
		self.without_money = """ + str(settings.without_money) + """
		self.categories = """ + str(settings.categories) + """
		self.search_string = '""" + design.builder.get_object("SearchEntry").get_text() + """'
		self.money_type = """ + str(settings.money_type) + """
		self.new_window = """ + str(settings.new_window) + """
		self.audio_notification = """ + str(settings.audio_notification) + """
		self.audio_notification_level = """ + str(settings.audio_notification_level)

design = FviewerDesign()
fviewer = Fviewer()
projects = Projects()
fviewer.start()

gtk.main()
